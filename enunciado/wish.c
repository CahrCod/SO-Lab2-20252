#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_TOKENS 100
#define MAX_PATHS 100

char error_message[] = "An error has occurred\n";

void print_error() {
    write(STDERR_FILENO, error_message, strlen(error_message));
}

// elimina espacios en blanco al inicio y al final
char *trim(char *s) {
    if (!s) return s;
    while (*s && (*s == ' ' || *s == '\t' || *s == '\n')) s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n')) {
        *end = '\0';
        end--;
    }
    return s;
}

// divide el comando por espacios en un arreglo argv (modificable)
int split_tokens(char *cmd, char **tokens) {
    int i = 0;
    char *saveptr;
    char *tok = strtok_r(cmd, " \t\n", &saveptr);
    while (tok != NULL && i < MAX_TOKENS-1) {
        tokens[i++] = tok;
        tok = strtok_r(NULL, " \t\n", &saveptr);
    }
    tokens[i] = NULL;
    return i;
}

int main(int argc, char *argv[]) {
    FILE *input = stdin;
    int batch = 0;
    if (argc > 2) {
        print_error();
        exit(1);
    }
    if (argc == 2) {
        input = fopen(argv[1], "r");
        if (!input) {
            print_error();
            exit(1);
        }
        batch = 1;
    }

    // ruta inicial: /bin
    char *paths[MAX_PATHS];
    int path_count = 0;
    paths[path_count++] = strdup("/bin");

    size_t linecap = 0;
    char *line = NULL;

    while (1) {
        if (!batch) {
            printf("wish> ");
            fflush(stdout);
        }
        ssize_t linelen = getline(&line, &linecap, input);
        if (linelen == -1) {
            // EOF (fin de archivo)
            exit(0);
        }
        // procesar la línea: comandos separados por '&'
        char *linecpy = strdup(line);
        char *saveptr1;
        char *cmdpart = strtok_r(linecpy, "&", &saveptr1);
        // recolectar PIDs de hijos para esperar
        pid_t pids[MAX_TOKENS];
        int pid_count = 0;

        while (cmdpart != NULL) {
            char *cmd = trim(cmdpart);
            if (strlen(cmd) == 0) {
                cmdpart = strtok_r(NULL, "&", &saveptr1);
                continue;
            }

            // comprobar redirección '>'
            char *redir = strchr(cmd, '>');
            char *outfile = NULL;
            int redirect = 0;
            if (redir) {
                // si no hay comando antes de '>' es un error
                int left_empty = 1;
                for (char *p = cmd; p < redir; p++) {
                    if (*p != ' ' && *p != '\t' && *p != '\n') { left_empty = 0; break; }
                }
                if (left_empty) {
                    print_error();
                    goto next_cmd;
                }
                redirect = 1;
                // asegurar que solo exista un '>'
                if (strchr(redir+1, '>') != NULL) {
                    print_error();
                    goto next_cmd;
                }
                *redir = '\0';
                outfile = trim(redir + 1);
                if (outfile == NULL || strlen(outfile) == 0) {
                    print_error();
                    goto next_cmd;
                }
                // asegurar que el nombre de archivo no contenga espacios
                // (los tests esperan un único token después de '>')
                char *tmp = strdup(outfile);
                char *toks[MAX_TOKENS];
                int nt = split_tokens(tmp, toks);
                free(tmp);
                if (nt != 1) {
                    print_error();
                    goto next_cmd;
                }
            }

            // tokenizar el comando
            char *cmddup = strdup(cmd);
            char *tokens[MAX_TOKENS];
            int tokcount = split_tokens(cmddup, tokens);
            if (tokcount == 0) {
                free(cmddup);
                goto next_cmd;
            }

            // manejar comandos internos: exit, cd, path
            if (strcmp(tokens[0], "exit") == 0) {
                if (tokcount != 1) {
                    print_error();
                } else {
                    exit(0);
                }
                free(cmddup);
                goto next_cmd;
            } else if (strcmp(tokens[0], "cd") == 0) {
                if (tokcount != 2) {
                    print_error();
                } else {
                    if (chdir(tokens[1]) != 0) {
                        print_error();
                    }
                }
                free(cmddup);
                goto next_cmd;
            } else if (strcmp(tokens[0], "path") == 0) {
                // liberar rutas anteriores
                for (int i = 0; i < path_count; i++) free(paths[i]);
                path_count = 0;
                for (int i = 1; i < tokcount; i++) {
                    if (path_count < MAX_PATHS) {
                        paths[path_count++] = strdup(tokens[i]);
                    }
                }
                free(cmddup);
                goto next_cmd;
            }

            // comando externo: si el nombre contiene '/' ejecutar directamente
            // (soporta rutas relativas y absolutas). Si no, buscar en las rutas.
            char fullpath[1024];
            int found = 0;
            if (strchr(tokens[0], '/') != NULL) {
                // token contiene '/', intentar ejecutar tal cual
                if (access(tokens[0], X_OK) == 0) {
                    snprintf(fullpath, sizeof(fullpath), "%s", tokens[0]);
                    found = 1;
                } else {
                    print_error();
                    free(cmddup);
                    goto next_cmd;
                }
            } else {
                for (int i = 0; i < path_count; i++) {
                    snprintf(fullpath, sizeof(fullpath), "%s/%s", paths[i], tokens[0]);
                    if (access(fullpath, X_OK) == 0) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    print_error();
                    free(cmddup);
                    goto next_cmd;
                }
            }

            // crear proceso (fork) y ejecutar (exec)
            pid_t pid = fork();
            if (pid < 0) {
                print_error();
                free(cmddup);
                goto next_cmd;
            } else if (pid == 0) {
                // proceso hijo
                if (redirect) {
                    int fd = open(outfile, O_CREAT | O_WRONLY | O_TRUNC, S_IRWXU);
                    if (fd < 0) {
                        print_error();
                        exit(1);
                    }
                    if (dup2(fd, STDOUT_FILENO) < 0) {
                        print_error(); exit(1);
                    }
                    if (dup2(fd, STDERR_FILENO) < 0) { print_error(); exit(1); }
                    close(fd);
                }
                execv(fullpath, tokens);
                // si execv retorna, puede ser por ENOEXEC (archivo de texto)
                // en ese caso intentar ejecutar con /bin/sh pasando los mismos argumentos
                if (errno == ENOEXEC) {
                    // construir argv para /bin/sh: ["sh", fullpath, tokens[1], tokens[2], ..., NULL]
                    char *shargs[MAX_TOKENS + 2];
                    int si = 0;
                    shargs[si++] = "sh";
                    shargs[si++] = fullpath;
                    for (int j = 1; j < tokcount && si < MAX_TOKENS+1; j++) {
                        shargs[si++] = tokens[j];
                    }
                    shargs[si] = NULL;
                    execv("/bin/sh", shargs);
                }
                // si llegamos acá, hubo un error al ejecutar
                print_error();
                exit(1);
            } else {
                // padre: guardar PID para esperar después
                pids[pid_count++] = pid;
            }

            free(cmddup);
        next_cmd:
            cmdpart = strtok_r(NULL, "&", &saveptr1);
            continue;
        }

        // esperar a todos los procesos hijos creados para esta línea
        for (int i = 0; i < pid_count; i++) {
            waitpid(pids[i], NULL, 0);
        }

        free(linecpy);
    }

    free(line);
    return 0;
}
