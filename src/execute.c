#include "shell.h"

#include <fcntl.h>
#include <ctype.h>

#define MAX_COMMANDS 10
#define MAX_TOKENS 256
#define MAX_JOBS 128


// Structure for one command in a pipeline
typedef struct {
    char* args[MAXARGS + 1];
    char* input_file;
    char* output_file;
    int argc;
} Command;


// Structure for one background job
typedef struct {
    pid_t pid;
    char command[MAX_LEN];
} BackgroundJob;


// List of active background jobs
static BackgroundJob background_jobs[MAX_JOBS];
static int background_job_count = 0;


// Exit status of the most recent foreground command
static int last_exit_status = 0;


// Return the exit status of the most recently executed foreground command
int get_last_exit_status(void) {
    return last_exit_status;
}


// =====================================================================
// Feature 8: shell variables
// =====================================================================

#define MAX_VAR_NAME 64

// One shell variable (singly linked list node)
typedef struct Variable {
    char* name;
    char* value;
    struct Variable* next;
} Variable;


// Head of the variable list (variables stay in creation order)
static Variable* variable_list = NULL;


// Return 1 if the text is a valid variable name: [A-Za-z_][A-Za-z0-9_]*
static int is_valid_name(const char* name) {

    if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) {
        return 0;
    }

    for (int i = 1; name[i] != '\0'; i++) {

        if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) {
            return 0;
        }
    }

    return 1;
}


// Find a variable by name, or NULL
static Variable* find_variable(const char* name) {

    for (Variable* v = variable_list; v != NULL; v = v->next) {

        if (strcmp(v->name, name) == 0) {
            return v;
        }
    }

    return NULL;
}


// Value of a variable. Shell variables are checked first,
// then environment variables (such as HOME). Returns NULL if undefined.
static const char* lookup_variable_value(const char* name) {

    Variable* v = find_variable(name);

    if (v != NULL) {
        return v->value;
    }

    return getenv(name);
}


// Create a variable or update an existing one. Returns 0 on success.
static int set_variable(const char* name, const char* value) {

    char* value_copy = strdup(value);

    if (value_copy == NULL) {
        perror("strdup");
        return -1;
    }

    Variable* existing = find_variable(name);

    // Re-assignment: replace the old value
    if (existing != NULL) {
        free(existing->value);
        existing->value = value_copy;
        return 0;
    }

    // New variable: allocate a node and append it to the list
    Variable* node = malloc(sizeof(Variable));

    if (node == NULL) {
        perror("malloc");
        free(value_copy);
        return -1;
    }

    node->name = strdup(name);

    if (node->name == NULL) {
        perror("strdup");
        free(value_copy);
        free(node);
        return -1;
    }

    node->value = value_copy;
    node->next = NULL;

    if (variable_list == NULL) {
        variable_list = node;
    }
    else {

        Variable* tail = variable_list;

        while (tail->next != NULL) {
            tail = tail->next;
        }

        tail->next = node;
    }

    return 0;
}


// Print all shell variables (used by the set built-in)
static void print_variables(void) {

    if (variable_list == NULL) {
        printf("No shell variables defined.\n");
        return;
    }

    for (Variable* v = variable_list; v != NULL; v = v->next) {
        printf("%s=%s\n", v->name, v->value);
    }
}


// Return 1 if the word looks like NAME=value
static int is_assignment(const char* word) {

    if (!(isalpha((unsigned char)word[0]) || word[0] == '_')) {
        return 0;
    }

    int i = 1;

    while (isalnum((unsigned char)word[i]) || word[i] == '_') {
        i++;
    }

    return word[i] == '=';
}


// Store NAME=value (the word has already been checked by is_assignment)
static void perform_assignment(const char* word) {

    const char* equals = strchr(word, '=');
    size_t name_length = (size_t)(equals - word);

    char name[MAX_VAR_NAME + 1];

    if (name_length > MAX_VAR_NAME) {
        fprintf(stderr, "Variable name too long\n");
        last_exit_status = 1;
        return;
    }

    memcpy(name, word, name_length);
    name[name_length] = '\0';

    if (set_variable(name, equals + 1) == 0) {
        last_exit_status = 0;
    }
    else {
        last_exit_status = 1;
    }
}


// Append text to the argument buffer. Returns 0 on success.
static int append_text(char* buffer, int* length, const char* text) {

    size_t text_length = strlen(text);

    if ((size_t)*length + text_length >= MAX_LEN) {
        fprintf(stderr, "Argument too long\n");
        return -1;
    }

    memcpy(buffer + *length, text, text_length);
    *length += (int)text_length;

    return 0;
}


// Expand one variable reference. *cursor points at the '$' character and
// is advanced past the reference. The value is appended to buffer.
// Supported forms: $NAME, ${NAME}, $?
static int expand_variable(char* buffer, int* length, char** cursor) {

    char* p = *cursor + 1;
    char name[MAX_VAR_NAME + 1];
    char number[16];
    const char* value = NULL;

    if (*p == '?') {

        snprintf(number, sizeof(number), "%d", last_exit_status);
        value = number;
        p++;
    }
    else if (*p == '{') {

        char* close_brace = strchr(p, '}');

        if (close_brace == NULL) {
            fprintf(stderr, "Syntax error: missing '}'\n");
            return -1;
        }

        size_t name_length = (size_t)(close_brace - (p + 1));

        if (name_length == 0 || name_length > MAX_VAR_NAME) {
            fprintf(stderr, "Syntax error: bad variable name\n");
            return -1;
        }

        memcpy(name, p + 1, name_length);
        name[name_length] = '\0';

        if (!is_valid_name(name)) {
            fprintf(stderr, "Syntax error: bad variable name\n");
            return -1;
        }

        value = lookup_variable_value(name);
        p = close_brace + 1;
    }
    else if (isalpha((unsigned char)*p) || *p == '_') {

        size_t name_length = 0;

        while (isalnum((unsigned char)*p) || *p == '_') {

            if (name_length < MAX_VAR_NAME) {
                name[name_length++] = *p;
            }

            p++;
        }

        name[name_length] = '\0';
        value = lookup_variable_value(name);
    }
    else {

        // A lone '$' is kept as a normal character
        *cursor = p;
        return append_text(buffer, length, "$");
    }

    *cursor = p;

    // Undefined variables expand to an empty string
    return append_text(buffer, length, value != NULL ? value : "");
}


// Remove a completed process from the active jobs list
static void remove_background_job(pid_t pid) {

    for (int i = 0; i < background_job_count; i++) {

        if (background_jobs[i].pid == pid) {

            for (int j = i; j < background_job_count - 1; j++) {
                background_jobs[j] = background_jobs[j + 1];
            }

            background_job_count--;
            return;
        }
    }
}


// Add a process to the active jobs list
static int add_background_job(pid_t pid, const char* command) {

    if (background_job_count >= MAX_JOBS) {
        fprintf(stderr, "Maximum background jobs reached.\n");
        return -1;
    }

    background_jobs[background_job_count].pid = pid;

    snprintf(
        background_jobs[background_job_count].command,
        MAX_LEN,
        "%s",
        command
    );

    background_job_count++;
    return 0;
}


// Display active background jobs
static void print_background_jobs(void) {

    if (background_job_count == 0) {
        printf("No background jobs.\n");
        return;
    }

    for (int i = 0; i < background_job_count; i++) {

        printf(
            "[%d] PID %ld %s\n",
            i + 1,
            (long)background_jobs[i].pid,
            background_jobs[i].command
        );
    }
}


// Reap completed background processes without blocking
void reap_background_jobs(void) {

    int status;
    pid_t pid;

    while (1) {

        pid = waitpid(-1, &status, WNOHANG);

        if (pid > 0) {

            remove_background_job(pid);

            printf(
                "\n[completed] PID %ld\n",
                (long)pid
            );

            continue;
        }

        if (pid == -1 && errno == EINTR) {
            continue;
        }

        break;
    }
}


// Built-in commands
int handle_builtin(char** arglist) {

    // EXIT
    if (strcmp(arglist[0], "exit") == 0) {
        return 2;
    }

    // CD
    if (strcmp(arglist[0], "cd") == 0) {

        char* directory;

        if (arglist[1] == NULL) {
            directory = getenv("HOME");

            if (directory == NULL) {
                fprintf(stderr, "cd: HOME not set\n");
                last_exit_status = 1;
                return 1;
            }
        }
        else if (arglist[2] != NULL) {
            fprintf(stderr, "cd: too many arguments\n");
            last_exit_status = 1;
            return 1;
        }
        else {
            directory = arglist[1];
        }

        if (chdir(directory) != 0) {
            perror("cd");
            last_exit_status = 1;
        }
        else {
            last_exit_status = 0;
        }

        return 1;
    }

    // HELP
    if (strcmp(arglist[0], "help") == 0) {

        printf("\nBuilt-in Commands:\n");
        printf("  exit              - Exit the shell\n");
        printf("  cd <directory>    - Change current directory\n");
        printf("  help              - Display this help message\n");
        printf("  jobs              - Display active background jobs\n");
        printf("  history           - Display command history\n");
        printf("\nControl structure:\n");
        printf("  if <command>\n");
        printf("  then\n");
        printf("    <commands>\n");
        printf("  else              (optional)\n");
        printf("    <commands>\n");
        printf("  fi\n\n");
        printf("  set               - Display all shell variables\n");
        printf("  NAME=value        - Assign a shell variable\n");

        last_exit_status = 0;
        return 1;
    }

    // JOBS
    if (strcmp(arglist[0], "jobs") == 0) {

        // Refresh the list before displaying it
        reap_background_jobs();
        print_background_jobs();

        last_exit_status = 0;
        return 1;
    }

    // HISTORY
    if (strcmp(arglist[0], "history") == 0) {
        print_history();
        last_exit_status = 0;
        return 1;
    }

    // SET: display all shell variables
    if (strcmp(arglist[0], "set") == 0) {
        print_variables();
        last_exit_status = 0;
        return 1;
    }

    return 0;
}


// Display GNU Readline history
void print_history(void) {

    HIST_ENTRY** entries = history_list();

    if (entries == NULL) {
        return;
    }

    for (int i = 0; entries[i] != NULL; i++) {
        printf("%d  %s\n", history_base + i, entries[i]->line);
    }
}


// Free token memory
static void free_tokens(char* tokens[], int count) {

    for (int i = 0; i < count; i++) {
        free(tokens[i]);
    }
}


// Split a line into words and operators.
// - Single and double quotes may appear anywhere inside a word
//   (for example MSG="Hello, World!").
// - $NAME, ${NAME} and $? are expanded here, except inside single quotes.
// Returns the token count, or -1 on error (tokens are freed on error).
static int parse_tokens(char* line, char* tokens[]) {

    int count = 0;
    char* p = line;

    while (*p != '\0') {

        while (isspace((unsigned char)*p)) {
            p++;
        }

        if (*p == '\0') {
            break;
        }

        if (count >= MAX_TOKENS) {
            fprintf(stderr, "Too many tokens\n");
            free_tokens(tokens, count);
            return -1;
        }

        // Operators are separate tokens, even without spaces
        if (*p == '|' || *p == '<' || *p == '>' || *p == '&') {

            char operator_token[2] = {*p, '\0'};
            tokens[count] = strdup(operator_token);

            if (tokens[count] == NULL) {
                perror("strdup");
                free_tokens(tokens, count);
                return -1;
            }

            count++;
            p++;
            continue;
        }

        // Read one word (quotes and variables are processed as we go)
        char buffer[MAX_LEN];
        int length = 0;
        char quote = '\0';
        int has_word = 0;       // set by quotes so "" gives an empty argument

        while (*p != '\0') {

            if (quote == '\0') {

                // An unquoted space or operator ends the word
                if (isspace((unsigned char)*p) ||
                    *p == '|' || *p == '<' || *p == '>' || *p == '&') {
                    break;
                }

                // Opening quote
                if (*p == '\'' || *p == '"') {
                    quote = *p;
                    has_word = 1;
                    p++;
                    continue;
                }
            }
            else if (*p == quote) {

                // Closing quote
                quote = '\0';
                p++;
                continue;
            }

            // Variable expansion (not inside single quotes)
            if (*p == '$' && quote != '\'') {

                if (expand_variable(buffer, &length, &p) != 0) {
                    free_tokens(tokens, count);
                    return -1;
                }

                continue;
            }

            if (length >= MAX_LEN - 1) {
                fprintf(stderr, "Argument too long\n");
                free_tokens(tokens, count);
                return -1;
            }

            buffer[length++] = *p;
            p++;
        }

        if (quote != '\0') {
            fprintf(stderr, "Syntax error: unmatched quote\n");
            free_tokens(tokens, count);
            return -1;
        }

        buffer[length] = '\0';

        // An unquoted variable that expanded to nothing disappears
        if (length == 0 && !has_word) {
            continue;
        }

        tokens[count] = strdup(buffer);

        if (tokens[count] == NULL) {
            perror("strdup");
            free_tokens(tokens, count);
            return -1;
        }

        count++;
    }

    return count;
}


// Parse tokens into pipeline commands
static int parse_commands(
    char* tokens[],
    int token_count,
    Command commands[],
    int* command_count
) {

    int current = 0;

    *command_count = 1;

    memset(commands, 0, sizeof(Command) * MAX_COMMANDS);

    for (int i = 0; i < token_count; i++) {

        // Pipe operator
        if (strcmp(tokens[i], "|") == 0) {

            if (commands[current].argc == 0) {
                fprintf(stderr, "Syntax error near pipe\n");
                return -1;
            }

            if (current + 1 >= MAX_COMMANDS) {
                fprintf(stderr, "Too many commands in pipeline\n");
                return -1;
            }

            current++;
            (*command_count)++;
            continue;
        }

        // Input redirection
        if (strcmp(tokens[i], "<") == 0) {

            if (i + 1 >= token_count ||
                strcmp(tokens[i + 1], "|") == 0 ||
                strcmp(tokens[i + 1], "<") == 0 ||
                strcmp(tokens[i + 1], ">") == 0 ||
                strcmp(tokens[i + 1], "&") == 0 ||
                commands[current].input_file != NULL) {

                fprintf(stderr, "Syntax error with input redirection\n");
                return -1;
            }

            commands[current].input_file = tokens[++i];
            continue;
        }

        // Output redirection
        if (strcmp(tokens[i], ">") == 0) {

            if (i + 1 >= token_count ||
                strcmp(tokens[i + 1], "|") == 0 ||
                strcmp(tokens[i + 1], "<") == 0 ||
                strcmp(tokens[i + 1], ">") == 0 ||
                strcmp(tokens[i + 1], "&") == 0 ||
                commands[current].output_file != NULL) {

                fprintf(stderr, "Syntax error with output redirection\n");
                return -1;
            }

            commands[current].output_file = tokens[++i];
            continue;
        }

        // Normal command argument
        if (commands[current].argc >= MAXARGS) {
            fprintf(stderr, "Too many arguments\n");
            return -1;
        }

        commands[current].args[commands[current].argc++] = tokens[i];
        commands[current].args[commands[current].argc] = NULL;
    }

    if (commands[current].argc == 0) {
        fprintf(stderr, "Syntax error: missing command\n");
        return -1;
    }

    return 0;
}


// Execute a pipeline with optional redirection and background mode
static int execute_pipeline(
    Command commands[],
    int command_count,
    int background,
    const char* job_command
) {

    int pipes[MAX_COMMANDS - 1][2];
    pid_t pids[MAX_COMMANDS];

    // Create pipes
    for (int i = 0; i < command_count - 1; i++) {

        if (pipe(pipes[i]) == -1) {
            perror("pipe");

            for (int j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            last_exit_status = 1;
            return -1;
        }
    }

    // Create one process per command
    for (int i = 0; i < command_count; i++) {

        pid_t pid = fork();

        if (pid == -1) {

            perror("fork");

            // Close all pipe ends
            for (int j = 0; j < command_count - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            // Wait for children already started
            for (int j = 0; j < i; j++) {
                while (waitpid(pids[j], NULL, 0) == -1 && errno == EINTR) {
                    // Retry if interrupted
                }
            }

            last_exit_status = 1;
            return -1;
        }

        if (pid == 0) {

            // Connect input from previous pipe
            if (i > 0) {
                if (dup2(pipes[i - 1][0], STDIN_FILENO) == -1) {
                    perror("dup2 input pipe");
                    _exit(EXIT_FAILURE);
                }
            }

            // Connect output to next pipe
            if (i < command_count - 1) {
                if (dup2(pipes[i][1], STDOUT_FILENO) == -1) {
                    perror("dup2 output pipe");
                    _exit(EXIT_FAILURE);
                }
            }

            // Close all pipe descriptors in child
            for (int j = 0; j < command_count - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            // Input redirection overrides pipe input
            if (commands[i].input_file != NULL) {

                int fd = open(commands[i].input_file, O_RDONLY);

                if (fd == -1) {
                    perror(commands[i].input_file);
                    _exit(EXIT_FAILURE);
                }

                if (dup2(fd, STDIN_FILENO) == -1) {
                    perror("dup2 input");
                    close(fd);
                    _exit(EXIT_FAILURE);
                }

                close(fd);
            }

            // Output redirection overrides pipe output
            if (commands[i].output_file != NULL) {

                int fd = open(
                    commands[i].output_file,
                    O_WRONLY | O_CREAT | O_TRUNC,
                    0644
                );

                if (fd == -1) {
                    perror(commands[i].output_file);
                    _exit(EXIT_FAILURE);
                }

                if (dup2(fd, STDOUT_FILENO) == -1) {
                    perror("dup2 output");
                    close(fd);
                    _exit(EXIT_FAILURE);
                }

                close(fd);
            }

            execvp(commands[i].args[0], commands[i].args);

            // exec failed: 127 = command not found, 126 = cannot execute
            perror(commands[i].args[0]);
            _exit(errno == ENOENT ? 127 : 126);
        }

        pids[i] = pid;
    }

    // Parent closes all pipe descriptors
    for (int i = 0; i < command_count - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    // Background execution: register children and return immediately
    if (background) {

        for (int i = 0; i < command_count; i++) {

            if (add_background_job(pids[i], job_command) == 0) {

                printf("[background] PID %ld\n", (long)pids[i]);
            }
            else {

                // If the job list is full, wait for this child
                while (waitpid(pids[i], NULL, 0) == -1 && errno == EINTR) {
                    // Retry if interrupted
                }
            }
        }

        last_exit_status = 0;
        return 0;
    }

    // Foreground execution: wait for every child.
    // The exit status of the pipeline is the status of its last command.
    for (int i = 0; i < command_count; i++) {

        int status = 0;

        while (waitpid(pids[i], &status, 0) == -1) {

            if (errno == EINTR) {
                continue;
            }

            perror("waitpid");
            status = 1 << 8;
            break;
        }

        if (i == command_count - 1) {

            if (WIFEXITED(status)) {
                last_exit_status = WEXITSTATUS(status);
            }
            else if (WIFSIGNALED(status)) {
                last_exit_status = 128 + WTERMSIG(status);
            }
            else {
                last_exit_status = 1;
            }
        }
    }

    return 0;
}


// Execute one command, including its pipeline and redirection
static int execute_single_command(char* line) {

    char* tokens[MAX_TOKENS];

    // Ignore comment lines
    if (line[0] == '#') {
        return 0;
    }

    int token_count = parse_tokens(line, tokens);

    if (token_count < 0) {
        last_exit_status = 2;
        return 0;
    }

    if (token_count == 0) {
        return 0;
    }

    // Detect background execution (&)
    int background = 0;

    for (int i = 0; i < token_count; i++) {

        if (strcmp(tokens[i], "&") == 0) {

            // Ampersand is allowed only at the end
            if (i != token_count - 1) {

                fprintf(stderr, "Syntax error: & must be at the end\n");
                free_tokens(tokens, token_count);
                last_exit_status = 2;
                return 0;
            }

            background = 1;

            free(tokens[i]);
            token_count--;

            break;
        }
    }

    if (token_count == 0) {
        fprintf(stderr, "Syntax error: missing command\n");
        last_exit_status = 2;
        return 0;
    }

    Command commands[MAX_COMMANDS];
    int command_count = 0;

    // Parse commands into pipeline segments
    if (parse_commands(
            tokens,
            token_count,
            commands,
            &command_count
        ) != 0) {

        free_tokens(tokens, token_count);
        last_exit_status = 2;
        return 0;
    }

    int result = 0;

    // Variable assignment (NAME=value) is handled by the shell itself,
    // without fork(), because the variable must live in the shell process
    if (!background &&
        command_count == 1 &&
        commands[0].argc == 1 &&
        commands[0].input_file == NULL &&
        commands[0].output_file == NULL &&
        is_assignment(commands[0].args[0])) {

        perform_assignment(commands[0].args[0]);
        free_tokens(tokens, token_count);
        return 0;
    }

    // Built-ins execute in the parent shell when run in foreground
    if (!background &&
        command_count == 1 &&
        commands[0].input_file == NULL &&
        commands[0].output_file == NULL) {

        result = handle_builtin(commands[0].args);

        if (result != 0) {
            free_tokens(tokens, token_count);
            return result;
        }
    }

    // Execute foreground or background pipeline
    execute_pipeline(
        commands,
        command_count,
        background,
        line
    );

    free_tokens(tokens, token_count);
    return 0;
}


// Main entry point: split and execute semicolon-separated commands
int execute_line(char* line) {

    char* start = line;
    char quote = '\0';
    int result = 0;

    for (char* p = line; ; p++) {

        // A semicolon separates commands only outside quotes
        if (*p == '\0' || (*p == ';' && quote == '\0')) {

            char delimiter = *p;
            *p = '\0';

            // Remove leading whitespace
            while (isspace((unsigned char)*start)) {
                start++;
            }

            // Remove trailing whitespace
            char* end = start + strlen(start);

            while (end > start && isspace((unsigned char)end[-1])) {
                *--end = '\0';
            }

            // Execute non-empty command
            if (*start != '\0') {

                result = execute_single_command(start);

                // Stop processing the line if exit was entered
                if (result == 2) {
                    return 2;
                }
            }

            if (delimiter == '\0') {
                break;
            }

            start = p + 1;
            continue;
        }

        // Track quoted sections so semicolons inside quotes are ignored
        if (quote != '\0') {

            if (*p == quote) {
                quote = '\0';
            }
        }
        else if (*p == '\'' || *p == '"') {

            quote = *p;
        }
    }

    return 0;
}



// =====================================================================
// Feature 7: if-then-else-fi
// =====================================================================

// Skip leading whitespace
static const char* skip_spaces(const char* text) {

    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }

    return text;
}


// Return 1 if the first word of the line is "if"
int is_if_line(const char* line) {

    const char* p = skip_spaces(line);

    return strncmp(p, "if", 2) == 0 &&
           (p[2] == '\0' || isspace((unsigned char)p[2]));
}


// Return 1 if the line contains only the given keyword
static int is_keyword_line(const char* line, const char* keyword) {

    const char* p = skip_spaces(line);
    size_t length = strlen(keyword);

    if (strncmp(p, keyword, length) != 0) {
        return 0;
    }

    return *skip_spaces(p + length) == '\0';
}


// Return 1 if the line is only "fi"
int is_fi_line(const char* line) {
    return is_keyword_line(line, "fi");
}


// Find the then / else / fi lines that belong to the if at lines[start].
// Nested if blocks are skipped. Returns 0 on success, -1 on syntax error.
static int find_if_structure(
    char** lines,
    int start,
    int count,
    int inline_then,
    int* then_index,
    int* else_index,
    int* fi_index
) {

    int depth = 1;

    *then_index = inline_then ? start : -1;
    *else_index = -1;
    *fi_index = -1;

    for (int j = start + 1; j < count; j++) {

        if (is_if_line(lines[j])) {
            depth++;
            continue;
        }

        if (is_fi_line(lines[j])) {

            depth--;

            if (depth == 0) {
                *fi_index = j;
                break;
            }

            continue;
        }

        if (depth != 1) {
            continue;
        }

        if (is_keyword_line(lines[j], "then") && *then_index == -1) {
            *then_index = j;
        }
        else if (is_keyword_line(lines[j], "else") &&
                 *then_index != -1 &&
                 *else_index == -1) {
            *else_index = j;
        }
    }

    if (*fi_index == -1) {
        fprintf(stderr, "Syntax error: missing 'fi'\n");
        return -1;
    }

    if (*then_index == -1 || (!inline_then && *then_index != start + 1)) {
        fprintf(stderr, "Syntax error: expected 'then' after 'if'\n");
        return -1;
    }

    return 0;
}


// Execute an array of lines, handling if blocks recursively
int execute_block(char** lines, int count) {

    int i = 0;

    while (i < count) {

        // Stray keywords outside an if block
        if (is_keyword_line(lines[i], "then") ||
            is_keyword_line(lines[i], "else") ||
            is_keyword_line(lines[i], "fi")) {

            fprintf(
                stderr,
                "Syntax error near unexpected token '%s'\n",
                skip_spaces(lines[i])
            );

            last_exit_status = 2;
            return 0;
        }

        // Ordinary command line
        if (!is_if_line(lines[i])) {

            char* copy = strdup(lines[i]);

            if (copy == NULL) {
                perror("strdup");
                return 0;
            }

            int result = execute_line(copy);
            free(copy);

            if (result == 2) {
                return 2;
            }

            i++;
            continue;
        }

        // ---- if block ----

        // Condition text: everything after the word "if"
        char* condition = strdup(skip_spaces(lines[i]) + 2);

        if (condition == NULL) {
            perror("strdup");
            return 0;
        }

        // Support "if command; then" on a single line
        int inline_then = 0;
        size_t length = strlen(condition);

        while (length > 0 && isspace((unsigned char)condition[length - 1])) {
            condition[--length] = '\0';
        }

        if (length >= 4 && strcmp(condition + length - 4, "then") == 0) {

            size_t k = length - 4;

            while (k > 0 && isspace((unsigned char)condition[k - 1])) {
                k--;
            }

            if (k > 0 && condition[k - 1] == ';') {
                condition[k - 1] = '\0';
                inline_then = 1;
            }
        }

        if (*skip_spaces(condition) == '\0') {
            fprintf(stderr, "Syntax error: missing condition after 'if'\n");
            free(condition);
            last_exit_status = 2;
            return 0;
        }

        int then_index, else_index, fi_index;

        if (find_if_structure(
                lines,
                i,
                count,
                inline_then,
                &then_index,
                &else_index,
                &fi_index
            ) != 0) {

            free(condition);
            last_exit_status = 2;
            return 0;
        }

        // Run the condition and read its exit status
        int result = execute_line(condition);
        free(condition);

        if (result == 2) {
            return 2;
        }

        int condition_status = get_last_exit_status();

        // Select the block to run
        int block_start;
        int block_end;

        if (condition_status == 0) {

            // then block: after "then" up to "else" (or "fi")
            block_start = then_index + 1;
            block_end = (else_index != -1) ? else_index : fi_index;
        }
        else if (else_index != -1) {

            // else block: after "else" up to "fi"
            block_start = else_index + 1;
            block_end = fi_index;
        }
        else {

            // No else block: nothing to run
            block_start = block_end = fi_index;
        }

        if (block_end > block_start) {

            result = execute_block(lines + block_start, block_end - block_start);

            if (result == 2) {
                return 2;
            }
        }

        // Continue after the matching "fi"
        i = fi_index + 1;
    }

    return 0;
}
