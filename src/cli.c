#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <direct.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#include "storj.h"

#define STORJ_THREADPOOL_SIZE "64"

#define CLI_NO_SUCH_FILE_OR_DIR   0x00
#define CLI_VALID_REGULAR_FILE    0x01
#define CLI_VALID_DIR             0x02
#define CLI_UNKNOWN_FILE_ATTR     0x03
#define CLI_UPLOAD_FILE_LOG_ERR   0x04

typedef struct {
    char *user;
    char *pass;
    char *host;
    char *mnemonic;
    char *key;
} user_options_t;

typedef struct {
  storj_env_t *env;
  char *bucket_name;
  char *bucket_id;
  char *file_name;      /**< next file ready to upload */
  char *file_path;      /**< next file ready to upload */
  char *file_id;        /**< file id of from the bridge */
  FILE *file_fd;        /**< upload file list fd */
  int   total_files;    /**< total files to upload */
  int   curr_up_file;   /**< current file number in uploadinng */
  char *curr_cmd_req;   /**< cli curr command requested */
  char *next_cmd_req;   /**< cli next command requested */
  bool  cmd_resp;       /**< cli command response 0->fail; 1->success */
  bool  file_xfer_stat; /**< false -> inprogress; true -> done */
  void *handle;
}cli_state_t;

#ifndef errno
extern int errno;
#endif

static void printdir(char *dir, int depth, FILE *fd);
static void queue_next_cli_cmd(cli_state_t *cli_state);
static int cli_upload_file(char *path, char *bucket_name, cli_state_t *cli_state);
static int cli_download_file(char *path, char *bucket_name, cli_state_t *cli_state);
static const char *get_filename_separator(const char *file_path);
static inline void noop() {};

#define HELP_TEXT "usage: storj [<options>] <command> [<args>]\n\n"     \
    "These are common Storj commands for various situations:\n\n"       \
    "setting up users profiles\n"                                       \
    "  register                  setup a new storj bridge user\n"       \
    "  import-keys               import existing user\n"                \
    "  export-keys               export bridge user, password and "     \
    "encryption keys\n\n"                                               \
    "working with buckets and files\n"                                  \
    "  list-buckets\n"                                                  \
    "  get-bucket-id <bucket-name>\n"                                   \
    "  list-files <bucket-name>\n"                                      \
    "  remove-file <bucket-id> <file-id>\n"                             \
    "  add-bucket <name> \n"                                            \
    "  remove-bucket <bucket-id>\n"                                     \
    "  list-mirrors <bucket-id> <file-id>\n\n"                          \
    "downloading and uploading files\n"                                 \
    "  upload-file <bucket-name> <path>\n"                              \
    "  cp <path-to-local-file-name> storj://<bucketname>/<file-name>"   \
    "  download-file <bucket-name> <file-name> <path>\n"                \
    "  cp storj://<bucketname>/<file-name> <path-to-local-file-name> "  \
    "bridge api information\n"                                          \
    "  get-info\n\n"                                                    \
    "options:\n"                                                        \
    "  -h, --help                output usage information\n"            \
    "  -v, --version             output the version number\n"           \
    "  -u, --url <url>           set the base url for the api\n"        \
    "  -p, --proxy <url>         set the socks proxy "                  \
    "(e.g. <[protocol://][user:password@]proxyhost[:port]>)\n"          \
    "  -l, --log <level>         set the log level (default 0)\n"       \
    "  -d, --debug               set the debug log level\n\n"           \
    "environment variables:\n"                                          \
    "  STORJ_KEYPASS             imported user settings passphrase\n"   \
    "  STORJ_BRIDGE              the bridge host "                      \
    "(e.g. https://api.storj.io)\n"                                     \
    "  STORJ_BRIDGE_USER         bridge username\n"                     \
    "  STORJ_BRIDGE_PASS         bridge password\n"                     \
    "  STORJ_ENCRYPTION_KEY      file encryption key\n\n"


#define CLI_VERSION "libstorj-2.0.1-beta"


static void print_error(char *this, char *filename1, char *filename2)
{
    fprintf(stderr, "%s cannot move %s to %s\n%s\n",
            this, filename1, filename2, strerror(errno));

    exit(EXIT_FAILURE);
}

static void print_upload_usage(char *this)
{
    fprintf(stderr,"SYNTAX ERROR:\nUsage %s [old_filename] [new_filename]",this);

    exit(EXIT_FAILURE);
}

static int check_file_path(char *file_path)
{
    struct stat sb;

    if (stat(file_path, &sb) == -1)
    {
        perror("stat");
        return CLI_NO_SUCH_FILE_OR_DIR;
    }

    switch (sb.st_mode & S_IFMT)
    {
        case S_IFBLK:
            printf("block device\n");
            break;
        case S_IFCHR:
            printf("character device\n");
            break;
        case S_IFDIR:
            return CLI_VALID_DIR;
            break;
        case S_IFIFO:
            printf("FIFO/pipe\n");
            break;
        case S_IFLNK:
            printf("symlink\n");
            break;
        case S_IFREG:
            return CLI_VALID_REGULAR_FILE;
            break;
        case S_IFSOCK:
            printf("socket\n");
            break;
        default:
            printf("unknown?\n");
            break;
    }

    #if ENABLE_FILE_DETAILS
    printf("I-node number:            %ld\n", (long)sb.st_ino);

    printf("Mode:                     %lo (octal)\n",
           (unsigned long)sb.st_mode);

    printf("Link count:               %ld\n", (long)sb.st_nlink);
    printf("Ownership:                UID=%ld   GID=%ld\n",
           (long)sb.st_uid, (long)sb.st_gid);

    printf("Preferred I/O block size: %ld bytes\n",
           (long)sb.st_blksize);
    printf("File size:                %lld bytes\n",
           (long long)sb.st_size);
    printf("Blocks allocated:         %lld\n",
           (long long)sb.st_blocks);

    printf("Last status change:       %s", ctime(&sb.st_ctime));
    printf("Last file access:         %s", ctime(&sb.st_atime));
    printf("Last file modification:   %s", ctime(&sb.st_mtime));
    #endif

    return CLI_UNKNOWN_FILE_ATTR;
}

static int file_exists(char *file_path)
{
    struct stat sb;
    gid_t  st_grpid;
    FILE *out_fd;

    if (stat(file_path, &sb) == -1)
    {
        perror("stat");
        return CLI_NO_SUCH_FILE_OR_DIR;
    }

    switch (sb.st_mode & S_IFMT)
    {
        case S_IFBLK:
            printf("block device\n");
            break;
        case S_IFCHR:
            printf("character device\n");
            break;
        case S_IFDIR:
            printf("file_path = %s\n\n", file_path);
            printf("directory\n");

            if((out_fd = fopen("output.txt", "w")) == NULL)
            {
                return CLI_UPLOAD_FILE_LOG_ERR;
            }
            printdir(file_path, 0, out_fd);
            fclose(out_fd);
            return CLI_VALID_DIR;
            break;
        case S_IFIFO:
            printf("FIFO/pipe\n");
            break;
        case S_IFLNK:
            printf("symlink\n");
            break;
        case S_IFREG:
            return CLI_VALID_REGULAR_FILE;
            break;
        case S_IFSOCK:
            printf("socket\n");
            break;
        default:
            printf("unknown?\n");
            break;
    }

    #if ENABLE_FILE_DETAILS
    printf("I-node number:            %ld\n", (long)sb.st_ino);

    printf("Mode:                     %lo (octal)\n",
           (unsigned long)sb.st_mode);

    printf("Link count:               %ld\n", (long)sb.st_nlink);
    printf("Ownership:                UID=%ld   GID=%ld\n",
           (long)sb.st_uid, (long)sb.st_gid);

    printf("Preferred I/O block size: %ld bytes\n",
           (long)sb.st_blksize);
    printf("File size:                %lld bytes\n",
           (long long)sb.st_size);
    printf("Blocks allocated:         %lld\n",
           (long long)sb.st_blocks);

    printf("Last status change:       %s", ctime(&sb.st_ctime));
    printf("Last file access:         %s", ctime(&sb.st_atime));
    printf("Last file modification:   %s", ctime(&sb.st_mtime));
    #endif

    return CLI_UNKNOWN_FILE_ATTR;
}

static int strpos(char *str, char *sub_str)
{
  /* find first        occurance of substring in string */
        char *sub_str_pos=strstr(str,sub_str);

  /* if null return -1 , otherwise return substring address - base address */
        return sub_str_pos == NULL ?  -1 :  (sub_str_pos - str );
}

static int validate_cmd_tokenize(char *cmd_str, char *str_token[])
{
    char sub_str[] = "storj://";
    int i = 0x00;   /* num of tokens */

    int ret = strpos(cmd_str, sub_str);
    if( ret == -1)
    {
        printf("Invalid Command Entry (%d), \ntry ... stroj://<bucket_name>/<file_name>\n", ret);
    }

    if (ret == 0x00)
    {
        /* start tokenizing */
        str_token[0] = strtok(cmd_str, "/");
        while (str_token[i] != NULL)
        {
            i++;
            str_token[i] = strtok(NULL, "/");
        }
    }
    else
    {
        i = ret;
    }

    return i;
}

static void printdir(char *dir, int depth, FILE *fd)
{
    DIR *dp;
    struct dirent *entry;
    struct stat statbuf;
    int spaces = depth*4;
    char tmp_dir[80];
    char *full_path = NULL;

    memset(tmp_dir, 0x00, sizeof(tmp_dir));

    if((dp = opendir(dir)) == NULL)
    {
        fprintf(stderr,"cannot open directory: %s\n", dir);
        return;
    }

    chdir(dir);
    while((entry = readdir(dp)) != NULL)
    {
        lstat(entry->d_name,&statbuf);
        if(S_ISDIR(statbuf.st_mode))
        {
            /* Found a directory, but ignore . and .. */
            if(strcmp(".",entry->d_name) == 0 ||
                strcmp("..",entry->d_name) == 0)
                continue;

            /* Recurse at a new indent level */
            printdir(entry->d_name,depth+1,fd);
        }
        else
        {
            full_path = realpath(entry->d_name, NULL);
            fprintf(fd,"%s%s\n","",full_path);
            //printf("%s%s\n","",full_path);
        }
    }
    chdir("..");
    closedir(dp);
    free(full_path);
}

static void json_logger(const char *message, int level, void *handle)
{
    printf("{\"message\": \"%s\", \"level\": %i, \"timestamp\": %" PRIu64 "}\n",
           message, level, storj_util_timestamp());
}

static char *get_home_dir()
{
#ifdef _WIN32
    return getenv("USERPROFILE");
#else
    return getenv("HOME");
#endif
}

static int make_user_directory(char *path)
{
    struct stat st = {0};
    if (stat(path, &st) == -1) {
#if _WIN32
        int mkdir_status = _mkdir(path);
        if (mkdir_status) {
            printf("Unable to create directory %s: code: %i.\n",
                   path,
                   mkdir_status);
            return 1;
        }
#else
        if (mkdir(path, 0700)) {
            printf("Unable to create directory %s: reason: %s\n",
                   path,
                   strerror(errno));
            return 1;
        }
#endif
    }
    return 0;
}

static const char *get_filename_separator(const char *file_path)
{
    const char *file_name = NULL;
#ifdef _WIN32
    file_name = strrchr(file_path, '\\');
    if (!file_name) {
        file_name = strrchr(file_path, '/');
    }
    if (!file_name && file_path) {
        file_name = file_path;
    }
    if (!file_name) {
        return NULL;
    }
    if (file_name[0] == '\\' || file_name[0] == '/') {
        file_name++;
    }
#else
    file_name = strrchr(file_path, '/');
    if (!file_name && file_path) {
        file_name = file_path;
    }
    if (!file_name) {
        return NULL;
    }
    if (file_name[0] == '/') {
        file_name++;
    }
#endif
    return file_name;
}

static int get_user_auth_location(char *host, char **root_dir, char **user_file)
{
    char *home_dir = get_home_dir();
    if (home_dir == NULL) {
        return 1;
    }

    int len = strlen(home_dir) + strlen("/.storj/");
    *root_dir = calloc(len + 1, sizeof(char));
    if (!*root_dir) {
        return 1;
    }

    strcpy(*root_dir, home_dir);
    strcat(*root_dir, "/.storj/");

    len = strlen(*root_dir) + strlen(host) + strlen(".json");
    *user_file = calloc(len + 1, sizeof(char));
    if (!*user_file) {
        return 1;
    }

    strcpy(*user_file, *root_dir);
    strcat(*user_file, host);
    strcat(*user_file, ".json");

    return 0;
}

static void get_input(char *line)
{
    if (fgets(line, BUFSIZ, stdin) == NULL) {
        line[0] = '\0';
    } else {
        int len = strlen(line);
        if (len > 0) {
            char *last = strrchr(line, '\n');
            if (last) {
                last[0] = '\0';
            }
            last = strrchr(line, '\r');
            if (last) {
                last[0] = '\0';
            }
        }
    }
}

static int generate_mnemonic(char **mnemonic)
{
    char *strength_str = NULL;
    int strength = 0;
    int status = 0;

    printf("We now need to create an secret key used for encrypting " \
           "files.\nPlease choose strength from: 128, 160, 192, 224, 256\n\n");

    while (strength % 32 || strength < 128 || strength > 256) {
        strength_str = calloc(BUFSIZ, sizeof(char));
        printf("Strength: ");
        get_input(strength_str);

        if (strength_str != NULL) {
            strength = atoi(strength_str);
        }

        free(strength_str);
    }

    if (*mnemonic) {
        free(*mnemonic);
    }

    *mnemonic = NULL;

    int generate_code = storj_mnemonic_generate(strength, mnemonic);
    if (*mnemonic == NULL || generate_code == 0) {
        printf("Failed to generate encryption key.\n");
        status = 1;
        status = generate_mnemonic(mnemonic);
    }

    return status;
}

static int get_password(char *password, int mask)
{
    int max_pass_len = 512;

#ifdef _WIN32
    HANDLE hstdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    DWORD prev_mode = 0;
    GetConsoleMode(hstdin, &mode);
    GetConsoleMode(hstdin, &prev_mode);
    SetConsoleMode(hstdin, mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT));
#else
    static struct termios prev_terminal;
    static struct termios terminal;

    tcgetattr(STDIN_FILENO, &prev_terminal);

    memcpy (&terminal, &prev_terminal, sizeof(struct termios));
    terminal.c_lflag &= ~(ICANON | ECHO);
    terminal.c_cc[VTIME] = 0;
    terminal.c_cc[VMIN] = 1;
    tcsetattr(STDIN_FILENO, TCSANOW, &terminal);
#endif

    size_t idx = 0;         /* index, number of chars in read   */
    int c = 0;

    const char BACKSPACE = 8;
    const char RETURN = 13;

    /* read chars from fp, mask if valid char specified */
#ifdef _WIN32
    long unsigned int char_read = 0;
    while ((ReadConsole(hstdin, &c, 1, &char_read, NULL) && c != '\n' && c != RETURN && c != EOF && idx < max_pass_len - 1) ||
            (idx == max_pass_len - 1 && c == BACKSPACE))
#else
    while (((c = fgetc(stdin)) != '\n' && c != EOF && idx < max_pass_len - 1) ||
            (idx == max_pass_len - 1 && c == 127))
#endif
    {
        if (c != 127 && c != BACKSPACE) {
            if (31 < mask && mask < 127)    /* valid ascii char */
                fputc(mask, stdout);
            password[idx++] = c;
        } else if (idx > 0) {         /* handle backspace (del)   */
            if (31 < mask && mask < 127) {
                fputc(0x8, stdout);
                fputc(' ', stdout);
                fputc(0x8, stdout);
            }
            password[--idx] = 0;
        }
    }
    password[idx] = 0; /* null-terminate   */

    // go back to the previous settings
#ifdef _WIN32
    SetConsoleMode(hstdin, prev_mode);
#else
    tcsetattr(STDIN_FILENO, TCSANOW, &prev_terminal);
#endif

    return idx; /* number of chars in passwd    */
}

static int get_password_verify(char *prompt, char *password, int count)
{
    printf("%s", prompt);
    char first_password[BUFSIZ];
    get_password(first_password, '*');

    printf("\nAgain to verify: ");
    char second_password[BUFSIZ];
    get_password(second_password, '*');

    int match = strcmp(first_password, second_password);
    strncpy(password, first_password, BUFSIZ);

    if (match == 0) {
        return 0;
    } else {
        printf("\nPassphrases did not match. ");
        count++;
        if (count > 3) {
            printf("\n");
            return 1;
        }
        printf("Try again...\n");
        return get_password_verify(prompt, password, count);
    }
}

static void close_signal(uv_handle_t *handle)
{
    ((void)0);
}

static void file_progress(double progress,
                          uint64_t downloaded_bytes,
                          uint64_t total_bytes,
                          void *handle)
{
    int bar_width = 70;

    if (progress == 0 && downloaded_bytes == 0) {
        printf("Preparing File...");
        fflush(stdout);
        return;
    }

    printf("\r[");
    int pos = bar_width * progress;
    for (int i = 0; i < bar_width; ++i) {
        if (i < pos) {
            printf("=");
        } else if (i == pos) {
            printf(">");
        } else {
            printf(" ");
        }
    }
    printf("] %.*f%%", 2, progress * 100);

    fflush(stdout);
}

static void upload_file_complete(int status, storj_file_meta_t *file, void *handle)
{
    cli_state_t *cli_state = handle;
    printf("\n");
    if (status != 0)
    {
        printf("Upload failure: %s\n", storj_strerror(status));
        //exit(status);
    }

    printf("Upload Success! File ID: %s\n", file->id);

    storj_free_uploaded_file_info(file);

    if((cli_state->total_files == 0x00) &&
       (cli_state->curr_up_file == 0x00))
    {
        exit(0);
    }
    queue_next_cli_cmd(handle);
}

static void upload_signal_handler(uv_signal_t *req, int signum)
{
    storj_upload_state_t *state = req->data;
    storj_bridge_store_file_cancel(state);
    if (uv_signal_stop(req)) {
        printf("Unable to stop signal\n");
    }
    uv_close((uv_handle_t *)req, close_signal);
}

static int upload_file(storj_env_t *env, char *bucket_id, const char *file_path, void *handle)
{
    FILE *fd = fopen(file_path, "r");

    if (!fd) {
        printf("Invalid file path: %s\n", file_path);
    }

    const char *file_name = get_filename_separator(file_path);

    if (!file_name) {
        file_name = file_path;
    }

    // Upload opts env variables:
    char *prepare_frame_limit = getenv("STORJ_PREPARE_FRAME_LIMIT");
    char *push_frame_limit = getenv("STORJ_PUSH_FRAME_LIMIT");
    char *push_shard_limit = getenv("STORJ_PUSH_SHARD_LIMIT");
    char *rs = getenv("STORJ_REED_SOLOMON");

    storj_upload_opts_t upload_opts = {
        .prepare_frame_limit = (prepare_frame_limit) ? atoi(prepare_frame_limit) : 1,
        .push_frame_limit = (push_frame_limit) ? atoi(push_frame_limit) : 64,
        .push_shard_limit = (push_shard_limit) ? atoi(push_shard_limit) : 64,
        .rs = (!rs) ? true : (strcmp(rs, "false") == 0) ? false : true,
        .bucket_id = bucket_id,
        .file_name = file_name,
        .fd = fd
    };

    uv_signal_t *sig = malloc(sizeof(uv_signal_t));
    if (!sig) {
        return 1;
    }
    uv_signal_init(env->loop, sig);
    uv_signal_start(sig, upload_signal_handler, SIGINT);



    storj_progress_cb progress_cb = (storj_progress_cb)noop;
    if (env->log_options->level == 0) {
        progress_cb = file_progress;
    }

    storj_upload_state_t *state = storj_bridge_store_file(env,
                                                          &upload_opts,
                                                          handle,
                                                          progress_cb,
                                                          upload_file_complete);

    if (!state) {
        return 1;
    }

    sig->data = state;

    return state->error_status;
}

static void download_file_complete(int status, FILE *fd, void *handle)
{
    cli_state_t *cli_state = handle;
    printf("\n");
    fclose(fd);
    if (status)
    {
        // TODO send to stderr
        switch(status) {
            case STORJ_FILE_DECRYPTION_ERROR:
                printf("Unable to properly decrypt file, please check " \
                       "that the correct encryption key was " \
                       "imported correctly.\n\n");
                break;
            default:
                printf("Download failure: %s\n", storj_strerror(status));
        }

        //exit(status);
    }
    else
    {
        printf("Download Success!\n");
    }

    if(cli_state->total_files == 0x00)
    {
        exit(0);
    }
    queue_next_cli_cmd(handle);
}

static void download_signal_handler(uv_signal_t *req, int signum)
{
    storj_download_state_t *state = req->data;
    storj_bridge_resolve_file_cancel(state);
    if (uv_signal_stop(req)) {
        printf("Unable to stop signal\n");
    }
    uv_close((uv_handle_t *)req, close_signal);
}

static int download_file(storj_env_t *env, char *bucket_id,
                         char *file_id, char *path, void *handle)
{
    FILE *fd = NULL;

    if (path) {
        char user_input[BUFSIZ];
        memset(user_input, '\0', BUFSIZ);

        if(access(path, F_OK) != -1 ) {
            printf("Warning: File already exists at path [%s].\n", path);
            while (strcmp(user_input, "y") != 0 && strcmp(user_input, "n") != 0)
            {
                memset(user_input, '\0', BUFSIZ);
                printf("Would you like to overwrite [%s]: [y/n] ", path);
                get_input(user_input);
            }

            if (strcmp(user_input, "n") == 0) {
                printf("\nCanceled overwriting of [%s].\n", path);
                return 1;
            }

            unlink(path);
        }

        fd = fopen(path, "w+");
    } else {
        fd = stdout;
    }

    if (fd == NULL) {
        // TODO send to stderr
        printf("Unable to open %s: %s\n", path, strerror(errno));
        return 1;
    }

    uv_signal_t *sig = malloc(sizeof(uv_signal_t));
    uv_signal_init(env->loop, sig);
    uv_signal_start(sig, download_signal_handler, SIGINT);

    storj_progress_cb progress_cb = (storj_progress_cb)noop;
    if (path && env->log_options->level == 0) {
        progress_cb = file_progress;
    }

    storj_download_state_t *state = storj_bridge_resolve_file(env, bucket_id,
                                                              file_id, fd, handle,
                                                              progress_cb,
                                                              download_file_complete);
    if (!state) {
        return 1;
    }
    sig->data = state;

    return state->error_status;
}

static void list_mirrors_callback(uv_work_t *work_req, int status)
{
    assert(status == 0);
    json_request_t *req = work_req->data;

    if (req->status_code != 200) {
        printf("Request failed with status code: %i\n",
               req->status_code);
    }

    if (req->response == NULL) {
        free(req);
        free(work_req);
        printf("Failed to list mirrors.\n");
        exit(1);
    }

    int num_mirrors = json_object_array_length(req->response);

    struct json_object *shard;
    struct json_object *established;
    struct json_object *available;
    struct json_object *item;
    struct json_object *hash;
    struct json_object *contract;
    struct json_object *address;
    struct json_object *port;
    struct json_object *node_id;

    for (int i = 0; i < num_mirrors; i++) {
        shard = json_object_array_get_idx(req->response, i);
        json_object_object_get_ex(shard, "established",
                                 &established);
        int num_established =
            json_object_array_length(established);
        for (int j = 0; j < num_established; j++) {
            item = json_object_array_get_idx(established, j);
            if (j == 0) {
                json_object_object_get_ex(item, "shardHash",
                                          &hash);
                printf("Shard %i: %s\n", i, json_object_get_string(hash));
            }
            json_object_object_get_ex(item, "contract", &contract);
            json_object_object_get_ex(contract, "farmer_id", &node_id);

            const char *node_id_str = json_object_get_string(node_id);
            printf("\tnodeID: %s\n", node_id_str);
        }
        printf("\n\n");
    }

    json_object_put(req->response);
    free(req->path);
    free(req);
    free(work_req);
}

static int import_keys(user_options_t *options)
{
    int status = 0;
    char *host = options->host ? strdup(options->host) : NULL;
    char *user = options->user ? strdup(options->user) : NULL;
    char *pass = options->pass ? strdup(options->pass) : NULL;
    char *key = options->key ? strdup(options->key) : NULL;
    char *mnemonic = options->mnemonic ? strdup(options->mnemonic): NULL;
    char *mnemonic_input = NULL;
    char *user_file = NULL;
    char *root_dir = NULL;
    int num_chars;

    char *user_input = calloc(BUFSIZ, sizeof(char));
    if (user_input == NULL) {
        printf("Unable to allocate buffer\n");
        status = 1;
        goto clear_variables;
    }

    if (get_user_auth_location(host, &root_dir, &user_file)) {
        printf("Unable to determine user auth filepath.\n");
        status = 1;
        goto clear_variables;
    }

    struct stat st;
    if (stat(user_file, &st) == 0) {
        printf("Would you like to overwrite the current settings?: [y/n] ");
        get_input(user_input);
        while (strcmp(user_input, "y") != 0 && strcmp(user_input, "n") != 0)
        {
            printf("Would you like to overwrite the current settings?: [y/n] ");
            get_input(user_input);
        }

        if (strcmp(user_input, "n") == 0) {
            printf("\nCanceled overwriting of stored credentials.\n");
            status = 1;
            goto clear_variables;
        }
    }

    if (!user) {
        printf("Bridge username (email): ");
        get_input(user_input);
        num_chars = strlen(user_input);
        user = calloc(num_chars + 1, sizeof(char));
        if (!user) {
            status = 1;
            goto clear_variables;
        }
        memcpy(user, user_input, num_chars * sizeof(char));
    }

    if (!pass) {
        printf("Bridge password: ");
        pass = calloc(BUFSIZ, sizeof(char));
        if (!pass) {
            status = 1;
            goto clear_variables;
        }
        get_password(pass, '*');
        printf("\n");
    }

    if (!mnemonic) {
        mnemonic_input = calloc(BUFSIZ, sizeof(char));
        if (!mnemonic_input) {
            status = 1;
            goto clear_variables;
        }

        printf("\nIf you've previously uploaded files, please enter your" \
               " existing encryption key (12 to 24 words). \nOtherwise leave" \
               " the field blank to generate a new key.\n\n");

        printf("Encryption key: ");
        get_input(mnemonic_input);
        num_chars = strlen(mnemonic_input);

        if (num_chars == 0) {
            printf("\n");
            generate_mnemonic(&mnemonic);
            printf("\n");

            printf("Encryption key: %s\n", mnemonic);
            printf("\n");
            printf("Please make sure to backup this key in a safe location. " \
                   "If the key is lost, the data uploaded will also be lost.\n\n");
        } else {
            mnemonic = calloc(num_chars + 1, sizeof(char));
            if (!mnemonic) {
                status = 1;
                goto clear_variables;
            }
            memcpy(mnemonic, mnemonic_input, num_chars * sizeof(char));
        }

        if (!storj_mnemonic_check(mnemonic)) {
            printf("Encryption key integrity check failed.\n");
            status = 1;
            goto clear_variables;
        }
    }

    if (!key) {
        key = calloc(BUFSIZ, sizeof(char));
        printf("We now need to save these settings. Please enter a passphrase" \
               " to lock your settings.\n\n");
        if (get_password_verify("Unlock passphrase: ", key, 0)) {
            printf("Unable to store encrypted authentication.\n");
            status = 1;
            goto clear_variables;
        }
        printf("\n");
    }

    if (make_user_directory(root_dir)) {
        status = 1;
        goto clear_variables;
    }

    if (storj_encrypt_write_auth(user_file, key, user, pass, mnemonic)) {
        status = 1;
        printf("Failed to write to disk\n");
        goto clear_variables;
    }

    printf("Successfully stored bridge username, password, and encryption "\
           "key to %s\n\n",
           user_file);

clear_variables:
    if (user) {
        free(user);
    }
    if (user_input) {
        free(user_input);
    }
    if (pass) {
        free(pass);
    }
    if (mnemonic) {
        free(mnemonic);
    }
    if (mnemonic_input) {
        free(mnemonic_input);
    }
    if (key) {
        free(key);
    }
    if (root_dir) {
        free(root_dir);
    }
    if (user_file) {
        free(user_file);
    }
    if (host) {
        free(host);
    }

    return status;
}

static void register_callback(uv_work_t *work_req, int status)
{
    assert(status == 0);
    json_request_t *req = work_req->data;

    if (req->status_code != 201) {
        printf("Request failed with status code: %i\n",
               req->status_code);
        struct json_object *error;
        json_object_object_get_ex(req->response, "error", &error);
        printf("Error: %s\n", json_object_get_string(error));

        user_options_t *handle = (user_options_t *) req->handle;
        free(handle->user);
        free(handle->host);
        free(handle->pass);
    } else {
        struct json_object *email;
        json_object_object_get_ex(req->response, "email", &email);
        printf("\n");
        printf("Successfully registered %s, please check your email "\
               "to confirm.\n", json_object_get_string(email));

        // save credentials
        char *mnemonic = NULL;
        printf("\n");
        generate_mnemonic(&mnemonic);
        printf("\n");

        printf("Encryption key: %s\n", mnemonic);
        printf("\n");
        printf("Please make sure to backup this key in a safe location. " \
               "If the key is lost, the data uploaded will also be lost.\n\n");

        user_options_t *user_opts = req->handle;

        user_opts->mnemonic = mnemonic;
        import_keys(user_opts);

        if (mnemonic) {
            free(mnemonic);
        }
        if (user_opts->pass) {
            free(user_opts->pass);
        }
        if (user_opts->user) {
            free(user_opts->user);
        }
        if (user_opts->host) {
            free(user_opts->host);
        }
    }

    json_object_put(req->response);
    json_object_put(req->body);
    free(req);
    free(work_req);
}

static void list_files_callback(uv_work_t *work_req, int status)
{
    int ret_status = 0;
    assert(status == 0);
    list_files_request_t *req = work_req->data;
    cli_state_t *cli_state = req->handle;

    if (req->status_code == 404) {
        printf("Bucket id [%s] does not exist\n", req->bucket_id);
        goto cleanup;
    } else if (req->status_code == 400) {
        printf("Bucket id [%s] is invalid\n", req->bucket_id);
        goto cleanup;
    } else if (req->status_code == 401) {
        printf("Invalid user credentials.\n");
        goto cleanup;
    } else if (req->status_code != 200) {
        printf("Request failed with status code: %i\n", req->status_code);
    }

    if (req->total_files == 0) {
        printf("No files for bucket.\n");
    }

    FILE *dwnld_list_fd = stdout;
    cli_state->file_id = NULL;

    if ((cli_state->file_name !=NULL) && (strcmp(cli_state->file_name,"*") == 0x00))
    {
        if ((dwnld_list_fd = fopen("dwnld_list.txt", "w")) == NULL)
        {
            printf("[%s][%d] Unable to create download list file \n",
                    __FUNCTION__, __LINE__);
            goto cleanup;
        }
        /* total number of files available in that bucket */
        cli_state->total_files = req->total_files;
    }

    for (int i = 0; i < req->total_files; i++)
    {
        storj_file_meta_t *file = &req->files[i];

        if (strcmp(cli_state->curr_cmd_req,"list-files") == 0x00)
        {
            /* print to screen */
            fprintf(stdout, "ID: %s \tSize: %" PRIu64 " bytes \tDecrypted: %s \tType: %s \tCreated: %s \tName: %s\n",
                   file->id,
                   file->size,
                   file->decrypted ? "true" : "false",
                   file->mimetype,
                   file->created,
                   file->filename);
        }

        /* print to file */
        if (dwnld_list_fd != stdout)
        {
            fprintf(dwnld_list_fd, "%s:%s\n",
                   file->id,
                   file->filename);
        }

        /* get the file id of the given file name */
        if(cli_state->file_name != NULL)
        {
            if (strcmp(cli_state->file_name, file->filename) == 0x00)
            {
                if((dwnld_list_fd != stdout) &&
                   (check_file_path("dwnld_list.txt") == CLI_VALID_REGULAR_FILE))
                {
                    if (remove("dwnld_list.txt") == 0x00)
                    {
                        printf("%s file deleted \n", "dwnld_list.txt");
                    }
                }
                cli_state->file_id = (char *)file->id;
                cli_state->next_cmd_req = "download-file-1";
                cli_state->total_files = 0x00;
            }
        }
    }

    if (dwnld_list_fd != stdout)
    {
        fclose(dwnld_list_fd);
    }

    if (strcmp(cli_state->curr_cmd_req, "download-file" ) == 0x00)
    {
        cli_state->curr_up_file = 0x01;
        cli_state->next_cmd_req = "download-file-1";
        queue_next_cli_cmd(cli_state);
    }
    else
    {
        if(check_file_path("dwnld_list.txt") == CLI_VALID_REGULAR_FILE)
        {
            if (remove("dwnld_list.txt") == 0x00)
            {
                printf("file deleted \n\n");
            }
        }
    }

cleanup:

    storj_free_list_files_request(req);
    free(work_req);
}

static void delete_file_callback(uv_work_t *work_req, int status)
{
    assert(status == 0);
    json_request_t *req = work_req->data;

    if (req->status_code == 200 || req->status_code == 204) {
        printf("File was successfully removed from bucket.\n");
    } else if (req->status_code == 401) {
        printf("Invalid user credentials.\n");
    } else {
        printf("Failed to remove file from bucket. (%i)\n", req->status_code);
    }

    json_object_put(req->response);
    free(req->path);
    free(req);
    free(work_req);
}

static void delete_bucket_callback(uv_work_t *work_req, int status)
{
    assert(status == 0);
    json_request_t *req = work_req->data;

    if (req->status_code == 200 || req->status_code == 204) {
        printf("Bucket was successfully removed.\n");
    } else if (req->status_code == 401) {
        printf("Invalid user credentials.\n");
    } else {
        printf("Failed to destroy bucket. (%i)\n", req->status_code);
    }

    json_object_put(req->response);
    free(req->path);
    free(req);
    free(work_req);
}

static void get_buckets_callback(uv_work_t *work_req, int status)
{
    assert(status == 0);
    get_buckets_request_t *req = work_req->data;

    if (req->status_code == 401) {
       printf("Invalid user credentials.\n");
    } else if (req->status_code != 200 && req->status_code != 304) {
        printf("Request failed with status code: %i\n", req->status_code);
    } else if (req->total_buckets == 0) {
        printf("No buckets.\n");
    }

    for (int i = 0; i < req->total_buckets; i++) {
        storj_bucket_meta_t *bucket = &req->buckets[i];
        printf("ID: %s \tDecrypted: %s \tCreated: %s \tName: %s\n",
               bucket->id, bucket->decrypted ? "true" : "false",
               bucket->created, bucket->name);
    }

    storj_free_get_buckets_request(req);
    free(work_req);
}

static void get_bucket_id_callback(uv_work_t *work_req, int status)
{
    int ret_status = 0x00;
    assert(status == 0);
    get_buckets_request_t *req = work_req->data;
    cli_state_t *cli_state = req->handle;

    if (req->status_code == 401) {
       printf("Invalid user credentials.\n");
    } else if (req->status_code != 200 && req->status_code != 304) {
        printf("Request failed with status code: %i\n", req->status_code);
    } else if (req->total_buckets == 0) {
        printf("No buckets.\n");
    }

    for (int i = 0; i < req->total_buckets; i++)
    {
        storj_bucket_meta_t *bucket = &req->buckets[i];
        cli_state->next_cmd_req = NULL;

        if (cli_state->bucket_name != NULL)
        {
            if(strcmp(cli_state->bucket_name, bucket->name) == 0x00)
            {
                printf("ID: %s \tDecrypted: %s \tCreated: %s \tName: %s\n",
                       bucket->id, bucket->decrypted ? "true" : "false",
                       bucket->created, bucket->name);
                cli_state->bucket_id = (char *)bucket->id;

                if(strcmp(cli_state->curr_cmd_req, "list-files") == 0x00)
                {
                    cli_state->next_cmd_req = "list-files-1";
                    ret_status = 1;
                }
                else if(strcmp(cli_state->curr_cmd_req, "download-file") == 0x00)
                {
                    cli_state->next_cmd_req = "list-files-1";
                    ret_status = 1;
                }
                else if(strcmp(cli_state->curr_cmd_req, "upload-file") == 0x00)
                {
                    cli_state->next_cmd_req = "upload-file-1";
                    ret_status = 1;
                }
                else if(strcmp(cli_state->curr_cmd_req, "get-bucket-id") == 0x00)
                {
                    ret_status = 0;
                }else
                {
                    printf("[%s][%d]Invalid curr cmd req = %s\n",
                           __FUNCTION__, __LINE__, cli_state->curr_cmd_req);
                    ret_status = 0;
                }
                break;
            }
            else
            {
                if (i >= (req->total_buckets -1))
                {
                    printf("Invalid bucket name. \n");
                }
            }
        }
        else
        {
            printf("ID: %s \tDecrypted: %s \tCreated: %s \tName: %s\n",
                   bucket->id, bucket->decrypted ? "true" : "false",
                   bucket->created, bucket->name);
        }
    }

    if(0x01 == ret_status)
    {
        queue_next_cli_cmd(cli_state);
    }

    storj_free_get_buckets_request(req);
    free(work_req);
}

static void create_bucket_callback(uv_work_t *work_req, int status)
{
    assert(status == 0);
    create_bucket_request_t *req = work_req->data;

    if (req->status_code == 404) {
        printf("Cannot create bucket [%s]. Name already exists \n", req->bucket->name);
        goto clean_variables;
    } else if (req->status_code == 401) {
        printf("Invalid user credentials.\n");
        goto clean_variables;
    }

    if (req->status_code != 201) {
        printf("Request failed with status code: %i\n", req->status_code);
        goto clean_variables;
    }

    if (req->bucket != NULL) {
        printf("ID: %s \tDecrypted: %s \tName: %s\n",
               req->bucket->id,
               req->bucket->decrypted ? "true" : "false",
               req->bucket->name);
    } else {
        printf("Failed to add bucket.\n");
    }

clean_variables:
    json_object_put(req->response);
    free((char *)req->encrypted_bucket_name);
    free(req->bucket);
    free(req);
    free(work_req);
}

static void get_info_callback(uv_work_t *work_req, int status)
{
    assert(status == 0);
    json_request_t *req = work_req->data;

    if (req->error_code || req->response == NULL) {
        free(req);
        free(work_req);
        if (req->error_code) {
            printf("Request failed, reason: %s\n",
                   curl_easy_strerror(req->error_code));
        } else {
            printf("Failed to get info.\n");
        }
        exit(1);
    }

    struct json_object *info;
    json_object_object_get_ex(req->response, "info", &info);

    struct json_object *title;
    json_object_object_get_ex(info, "title", &title);
    struct json_object *description;
    json_object_object_get_ex(info, "description", &description);
    struct json_object *version;
    json_object_object_get_ex(info, "version", &version);
    struct json_object *host;
    json_object_object_get_ex(req->response, "host", &host);

    printf("Title:       %s\n", json_object_get_string(title));
    printf("Description: %s\n", json_object_get_string(description));
    printf("Version:     %s\n", json_object_get_string(version));
    printf("Host:        %s\n", json_object_get_string(host));

    json_object_put(req->response);
    free(req);
    free(work_req);
}

static int export_keys(char *host)
{
    int status = 0;
    char *user_file = NULL;
    char *root_dir = NULL;
    char *user = NULL;
    char *pass = NULL;
    char *mnemonic = NULL;
    char *key = NULL;

    if (get_user_auth_location(host, &root_dir, &user_file)) {
        printf("Unable to determine user auth filepath.\n");
        status = 1;
        goto clear_variables;
    }

    if (access(user_file, F_OK) != -1) {
        key = calloc(BUFSIZ, sizeof(char));
        printf("Unlock passphrase: ");
        get_password(key, '*');
        printf("\n\n");

        if (storj_decrypt_read_auth(user_file, key, &user, &pass, &mnemonic)) {
            printf("Unable to read user file.\n");
            status = 1;
            goto clear_variables;
        }

        printf("Username:\t%s\nPassword:\t%s\nEncryption key:\t%s\n",
               user, pass, mnemonic);
    }

clear_variables:
    if (user) {
        free(user);
    }
    if (pass) {
        free(pass);
    }
    if (mnemonic) {
        free(mnemonic);
    }
    if (root_dir) {
        free(root_dir);
    }
    if (user_file) {
        free(user_file);
    }
    if (key) {
        free(key);
    }
    return status;
}

int main(int argc, char **argv)
{
    int status = 0;
    char temp_buff[256] = {};

    static struct option cmd_options[] = {
        {"url", required_argument,  0, 'u'},
        {"version", no_argument,  0, 'v'},
        {"proxy", required_argument,  0, 'p'},
        {"log", required_argument,  0, 'l'},
        {"debug", no_argument,  0, 'd'},
        {"help", no_argument,  0, 'h'},
        {"recursive", required_argument,  0, 'r'},
        {0, 0, 0, 0}
    };

    int index = 0;

    // The default is usually 4 threads, we want to increase to the
    // locally set default value.
#ifdef _WIN32
    if (!getenv("UV_THREADPOOL_SIZE")) {
        _putenv_s("UV_THREADPOOL_SIZE", STORJ_THREADPOOL_SIZE);
    }
#else
    setenv("UV_THREADPOOL_SIZE", STORJ_THREADPOOL_SIZE, 0);
#endif

    char *storj_bridge = getenv("STORJ_BRIDGE");
    int c;
    int log_level = 0;
    char *local_file_path = NULL;

    char *proxy = getenv("STORJ_PROXY");

    while ((c = getopt_long_only(argc, argv, "hdl:p:vVu:r:R:",
                                 cmd_options, &index)) != -1) {
        switch (c) {
            case 'u':
                storj_bridge = optarg;
                break;
            case 'p':
                proxy = optarg;
                break;
            case 'l':
                log_level = atoi(optarg);
                break;
            case 'd':
                log_level = 4;
                break;
            case 'V':
            case 'v':
                printf(CLI_VERSION "\n\n");
                exit(0);
                break;
            case 'R':
            case 'r':
                local_file_path = optarg;
                break;
            case 'h':
                printf(HELP_TEXT);
                exit(0);
                break;
            default:
                exit(0);
                break;

        }
    }

    if (log_level > 4 || log_level < 0) {
        printf("Invalid log level\n");
        return 1;
    }

    int command_index = optind;

    char *command = argv[command_index];
    if (!command) {
        printf(HELP_TEXT);
        return 0;
    }

    if (!storj_bridge) {
        storj_bridge = "https://api.storj.io:443/";
    }

    // Parse the host, part and proto from the storj bridge url
    char proto[6];
    char host[100];
    int port = 0;
    sscanf(storj_bridge, "%5[^://]://%99[^:/]:%99d", proto, host, &port);

    if (port == 0) {
        if (strcmp(proto, "https") == 0) {
            port = 443;
        } else {
            port = 80;
        }
    }

    if (strcmp(command, "login") == 0) {
        printf("'login' is not a storj command. Did you mean 'import-keys'?\n\n");
        return 1;
    }

    if (strcmp(command, "import-keys") == 0) {
        user_options_t user_options = {NULL, NULL, host, NULL, NULL};
        return import_keys(&user_options);
    }

    if (strcmp(command, "export-keys") == 0) {
        return export_keys(host);
    }

    // initialize event loop and environment
    storj_env_t *env = NULL;

    storj_http_options_t http_options = {
        .user_agent = CLI_VERSION,
        .low_speed_limit = STORJ_LOW_SPEED_LIMIT,
        .low_speed_time = STORJ_LOW_SPEED_TIME,
        .timeout = STORJ_HTTP_TIMEOUT
    };

    storj_log_options_t log_options = {
        .logger = json_logger,
        .level = log_level
    };

    if (proxy) {
        http_options.proxy_url = proxy;
    } else {
        http_options.proxy_url = NULL;
    }

    char *user = NULL;
    char *pass = NULL;
    char *mnemonic = NULL;
    cli_state_t *cli_state = NULL;
    storj_api_t *storj_api = NULL;

    if (strcmp(command, "get-info") == 0) {
        printf("Storj bridge: %s\n\n", storj_bridge);

        storj_bridge_options_t options = {
            .proto = proto,
            .host  = host,
            .port  = port,
            .user  = NULL,
            .pass  = NULL
        };

        env = storj_init_env(&options, NULL, &http_options, &log_options);
        if (!env) {
            return 1;
        }

        storj_bridge_get_info(env, NULL, get_info_callback);

    } else if(strcmp(command, "register") == 0) {
        storj_bridge_options_t options = {
            .proto = proto,
            .host  = host,
            .port  = port,
            .user  = NULL,
            .pass  = NULL
        };

        env = storj_init_env(&options, NULL, &http_options, &log_options);
        if (!env) {
            return 1;
        }

        user = calloc(BUFSIZ, sizeof(char));
        if (!user) {
            return 1;
        }
        printf("Bridge username (email): ");
        get_input(user);

        printf("Bridge password: ");
        pass = calloc(BUFSIZ, sizeof(char));
        if (!pass) {
            return 1;
        }
        get_password(pass, '*');
        printf("\n");

        user_options_t user_opts = {strdup(user), strdup(pass), strdup(host), NULL, NULL};

        if (!user_opts.user || !user_opts.host || !user_opts.pass) {
            return 1;
        }

        storj_bridge_register(env, user, pass, &user_opts, register_callback);
    } else {

        char *user_file = NULL;
        char *root_dir = NULL;
        if (get_user_auth_location(host, &root_dir, &user_file)) {
            printf("Unable to determine user auth filepath.\n");
            return 1;
        }

        // We aren't using root dir so free it
        free(root_dir);

        // First, get auth from environment variables
        user = getenv("STORJ_BRIDGE_USER") ?
            strdup(getenv("STORJ_BRIDGE_USER")) : NULL;

        pass = getenv("STORJ_BRIDGE_PASS") ?
            strdup(getenv("STORJ_BRIDGE_PASS")) : NULL;

        mnemonic = getenv("STORJ_ENCRYPTION_KEY") ?
            strdup(getenv("STORJ_ENCRYPTION_KEY")) : NULL;

        char *keypass = getenv("STORJ_KEYPASS");

        // Second, try to get from encrypted user file
        if ((!user || !pass || !mnemonic) && access(user_file, F_OK) != -1) {

            char *key = NULL;
            if (keypass) {
                key = calloc(strlen(keypass) + 1, sizeof(char));
                if (!key) {
                    return 1;
                }
                strcpy(key, keypass);
            } else {
                key = calloc(BUFSIZ, sizeof(char));
                if (!key) {
                    return 1;
                }
                printf("Unlock passphrase: ");
                get_password(key, '*');
                printf("\n");
            }
            char *file_user = NULL;
            char *file_pass = NULL;
            char *file_mnemonic = NULL;
            if (storj_decrypt_read_auth(user_file, key, &file_user,
                                        &file_pass, &file_mnemonic)) {
                printf("Unable to read user file. Invalid keypass or path.\n");
                free(key);
                free(user_file);
                free(file_user);
                free(file_pass);
                free(file_mnemonic);
                goto end_program;
            }
            free(key);
            free(user_file);

            if (!user && file_user) {
                user = file_user;
            } else if (file_user) {
                free(file_user);
            }

            if (!pass && file_pass) {
                pass = file_pass;
            } else if (file_pass) {
                free(file_pass);
            }

            if (!mnemonic && file_mnemonic) {
                mnemonic = file_mnemonic;
            } else if (file_mnemonic) {
                free(file_mnemonic);
            }
        }

        // Third, ask for authentication
        if (!user) {
            char *user_input = malloc(BUFSIZ);
            if (user_input == NULL) {
                return 1;
            }
            printf("Bridge username (email): ");
            get_input(user_input);
            int num_chars = strlen(user_input);
            user = calloc(num_chars + 1, sizeof(char));
            if (!user) {
                return 1;
            }
            memcpy(user, user_input, num_chars);
            free(user_input);
        }

        if (!pass) {
            printf("Bridge password: ");
            pass = calloc(BUFSIZ, sizeof(char));
            if (!pass) {
                return 1;
            }
            get_password(pass, '*');
            printf("\n");
        }

        if (!mnemonic) {
            printf("Encryption key: ");
            char *mnemonic_input = malloc(BUFSIZ);
            if (mnemonic_input == NULL) {
                return 1;
            }
            get_input(mnemonic_input);
            int num_chars = strlen(mnemonic_input);
            mnemonic = calloc(num_chars + 1, sizeof(char));
            if (!mnemonic) {
                return 1;
            }
            memcpy(mnemonic, mnemonic_input, num_chars);
            free(mnemonic_input);
            printf("\n");
        }

        storj_bridge_options_t options = {
            .proto = proto,
            .host  = host,
            .port  = port,
            .user  = user,
            .pass  = pass
        };

        storj_encrypt_options_t encrypt_options = {
            .mnemonic = mnemonic
        };

        env = storj_init_env(&options, &encrypt_options,
                             &http_options, &log_options);
        if (!env) {
            status = 1;
            goto end_program;
        }

        cli_state = malloc(sizeof(cli_state_t));

        if (!cli_state) {
            status = 1;
            goto end_program;
        }
        memset(cli_state, 0x00, sizeof(cli_state_t));

        cli_state->env = env;

        storj_api = malloc(sizeof(storj_api_t));

        if (!cli_state) {
            status = 1;
            goto end_program;
        }
        memset(storj_api, 0x00, sizeof(storj_api));

        storj_api->env = env;


        printf("command = %s; command_index = %d\n", command, command_index);
        printf("local_file_path (req arg_ = %s\n", local_file_path);
        for (int i = 0x00; i < argc; i++)
        {
            printf("argc = %d; argv[%d] = %s\n", argc, i, argv[i]);
        }

        for (int i = 0x00; i < (argc - command_index); i++)
        {
            printf("argc = %d; argv[command_index+%d] = %s\n", argc, i, argv[command_index + i]);
        }

        if (strcmp(command, "download-file") == 0)
        {

            /* get the corresponding bucket id from the bucket name */
            storj_api->bucket_name = argv[command_index + 1];
            storj_api->file_name = argv[command_index + 2];
            storj_api->dst_file = argv[command_index + 3];

            if (!storj_api->bucket_name || !storj_api->file_name)
            {
                printf("Missing arguments: <bucket-name> <file_name> <into_local_file_name>\n");
                status = 1;
                goto end_program;
            }
           
            storj_download_file(storj_api); 

            #if 0
            char *bucket_name = argv[command_index + 1];
            char *file_name = argv[command_index + 2];
            char *path = argv[command_index + 3];

            if (!bucket_name || !file_name || !path)
            {
                printf("Missing arguments: <bucket-name> <file-name> <path>\n");
                status = 1;
                goto end_program;
            }
            else
            {
                cli_state->curr_cmd_req = command;
                cli_state->bucket_name = bucket_name;
                cli_state->file_name = file_name;
                cli_state->file_path = path;
                if(!cli_state->bucket_id)
                {
                    storj_bridge_get_buckets(env, cli_state, get_bucket_id_callback);
                }
            }
            #endif
        }
        else if (strcmp(command, "cp") == 0)
        {
            #define UPLOAD_CMD          0x00
            #define DOWNLOAD_CMD        0x01
            #define RECURSIVE_CMD       0x02
            #define NON_RECURSIVE_CMD   0x03

            int ret = 0x00;
            char *src_path = NULL; /* holds the local path */
            char *dst_path = NULL; /* holds the storj:// path */
            char *bucket_name = NULL;
            int cmd_type = 0x00; /* 0-> upload and 1 -> download */

            /* cp command wrt to upload-file */
            if(local_file_path == NULL)/*  without -r[R] */
            { 
                /* hold the local path */
                src_path = argv[command_index + 0x01];

                /* Handle the dst argument (storj://<bucket-name>/ */
                dst_path = argv[argc - 0x01];
                printf("bucket-name = %s\n", bucket_name);

                cmd_type = NON_RECURSIVE_CMD;
            }
            else /* with -r[R] */
            {
                if ((strcmp(argv[1],"-r") == 0x00) || (strcmp(argv[1],"-R") == 0x00))
                {
                    src_path = local_file_path;

                    /* Handle the dst argument (storj://<bucket-name>/ */
                    dst_path = argv[argc - 0x01];

                    cmd_type = RECURSIVE_CMD;
                }
                else
                {
                    printf("[%s][%d] Invalid command option '%s'\n",
                           __FUNCTION__, __LINE__, argv[1]);
                    goto end_program;
                }
            }

            char sub_str[] = "storj://";

            /* check for upload or download command */
            ret = strpos(dst_path, sub_str);
            if( ret == 0x00) /* Handle upload command*/
            {
                if (cmd_type == NON_RECURSIVE_CMD)
                {
                    if (check_file_path(src_path) != CLI_VALID_DIR)
                    {
                        local_file_path = src_path;
                        printf("[%s][%d] local_file_path = %s\n",
                                __FUNCTION__, __LINE__, local_file_path);

                        bucket_name = dst_path;
                        printf("[%s][%d] bucket-name = %s\n", 
                               __FUNCTION__, __LINE__, bucket_name);
                    }
                    else
                    {
                        printf("[%s][%d] Invalid command entry\n",
                               __FUNCTION__, __LINE__);
                        goto end_program;
                    }
                }
                else if (cmd_type == RECURSIVE_CMD)
                {
                    local_file_path = src_path;
                    printf("[%s][%d] local_file_path = %s\n",
                            __FUNCTION__, __LINE__, local_file_path);

                    bucket_name = dst_path;
                    printf("[%s][%d] bucket-name = %s\n", 
                           __FUNCTION__, __LINE__, bucket_name);
                }
                else
                {
                    printf("[%s][%d] Invalid command entry \n", __FUNCTION__, __LINE__);
                    goto end_program;
                }

                printf("[main.c][1921] upload command\n");
                cmd_type = UPLOAD_CMD;
            }
            else if (ret == -1) /* Handle download command*/
            {
                ret = strpos(src_path, sub_str);
                 
                if (ret == 0x00)
                {
                    printf("[main.c][1941] download command\n");
                    cmd_type = DOWNLOAD_CMD;

                    local_file_path = dst_path;
                    printf("[main.c][1906] local_file_path = %s\n", local_file_path);

                    bucket_name = src_path;
                    printf("[main.c][1912] bucket-name = %s\n", bucket_name);
                }
                else
                {
                    printf("[%s][%d]Invalid Command Entry (%d), \ntry ... stroj://<bucket_name>/<file_name>\n", 
                           __FUNCTION__, __LINE__, ret);
                    goto end_program;
                }
            }
            else
            {
                printf("[%s][%d]Invalid Command Entry (%d), \ntry ... stroj://<bucket_name>/<file_name>\n", 
                       __FUNCTION__, __LINE__, ret);
                goto end_program;
            }

            /* handling single file copy with -r[R]: ./storj cp -r /home/kishore/libstorj/src/xxx.y storj://testbucket/yyy.x */
            /* Handle the src argument */
            /* single file to copy, make sure the files exits */
            if ((argc == 0x05) && (check_file_path(local_file_path) == CLI_VALID_REGULAR_FILE))
            {
                storj_api->file_name = local_file_path;

                /* Handle the dst argument (storj://<bucket-name>/<file-name> */
                /* token[0]-> storj:; token[1]->bucket_name; token[2]->upload_file_name */
                char *token[0x03];
                memset(token,0x00, sizeof(token));
                int num_of_tokens = validate_cmd_tokenize(bucket_name, token);

                if ((num_of_tokens == 0x02) || (num_of_tokens == 0x03))
                {
                    for (int j = 0x00; j < num_of_tokens; j++)
                    {
                        printf("num of tokes = %d; token[%d] = %s\n", num_of_tokens, j, token[j]);
                    }

                    char *dst_file_name = NULL;

                    storj_api->bucket_name = token[1];
                    dst_file_name = (char *)get_filename_separator(local_file_path);

                    if ((token[2] == NULL) || (strcmp(dst_file_name, token[2]) == 0x00) ||
                        (strcmp(token[2], ".") == 0x00))
                    {
                        /* use the src list buff as temp memory to hold the dst filename */
                        memset(storj_api->src_list, 0x00, sizeof(storj_api->src_list));
                        strcpy(storj_api->src_list, dst_file_name);
                        storj_api->dst_file = storj_api->src_list; 
                        printf("file uploaded as same %s \n", storj_api->dst_file);
                    } 
                    else
                    {
                        storj_api->dst_file = token[2];
                        printf("file uploaded as %s\n", storj_api->dst_file);
                    }
                    printf("******* EXECUTE UPLOAD CMD HERE \n");
                    storj_upload_file(storj_api);
                } 
                else
                {
                    printf("[%s][%d] Valid dst filename missing !!!!!\n", __FUNCTION__, __LINE__);
                    goto end_program;
                }
            } 
            else
            {
                /* directory is being used, store it in file_path */
                storj_api->file_path = local_file_path;

                char pwd_path[256]= {};
                memset(pwd_path, 0x00, sizeof(pwd_path));
                char *upload_list_file = pwd_path;

                /* create "/tmp/STORJ_upload_list_file.txt" upload files list based on the file path */
                if ((upload_list_file = getenv("TMPDIR")) != NULL)
                {
                    printf("uploadlistfile = %s\n", upload_list_file);
                    printf("upload_list_file[strlen(upload_list_file)] = %d\n", strlen(upload_list_file));
                    if (upload_list_file[(strlen(upload_list_file) - 1)] == '/')
                    {
                        strcat(upload_list_file, "STORJ_output_list.txt");
                    }
                    else
                    {
                        strcat(upload_list_file, "/STORJ_output_list.txt");
                    }

                    /* check the directory and create the path to upload list file */
                    memset(storj_api->src_list, 0x00, sizeof(storj_api->src_list));
                    memcpy(storj_api->src_list, upload_list_file, sizeof(pwd_path));
                    storj_api->dst_file = storj_api->src_list;
                    printf("**dst_file = %s\n", storj_api->dst_file);
                }
                else
                {
                    printf("[%s][%d] Upload list file generation error!!! \n",
                           __FUNCTION__, __LINE__);
                    goto end_program;
                }

                /* Handle wild character options for files selection */
                if(check_file_path(local_file_path) != CLI_VALID_DIR)
                {
                    /* if local file path is a file, then just get the directory
                       from that */
                    char *ret = NULL;
                    ret = strrchr(local_file_path, '/');
                    memset(temp_buff, 0x00, sizeof(temp_buff));
                    memcpy(temp_buff, local_file_path, (ret-local_file_path));

                    printf("ret = %s diff = %d temp = %s\n", ret, ret -local_file_path, temp_buff);

                    FILE *file= NULL;
                    /* create the file and add the list of files to be uploaded */
                    if ((file = fopen(storj_api->src_list, "w")) != NULL)
                    {
                        fprintf(file, "%s\n", local_file_path);
                        fprintf(stdout, "%s\n", local_file_path);

                        for (int i = 0x01; i < ((argc - command_index) - 1); i++)
                        {
                            fprintf(file, "%s\n", argv[command_index + i]);
                            fprintf(stdout, "%s\n", argv[command_index + i]);
                        }
                    }
                    else
                    {
                        printf("[%s][%d] Invalid upload src path entered\n", __FUNCTION__, __LINE__);
                        goto end_program;
                    }
                    fclose(file);

                    storj_api->file_path = temp_buff;
                    printf("src file path = %s\n", storj_api->file_path);
                }

                /* token[0]-> storj:; token[1]->bucket_name; token[2]->upload_file_name */
                char *token[0x03];
                memset(token, 0x00, sizeof(token));
                int num_of_tokens = validate_cmd_tokenize(bucket_name, token);
                printf("num of tokes = %d; \n", num_of_tokens);

                if ((num_of_tokens > 0x00) && ((num_of_tokens >= 0x02) || (num_of_tokens <= 0x03)))
                {
                    for (int j = 0x00; j < num_of_tokens; j++)
                    {
                        printf("num of tokes = %d; token[%d] = %s\n", num_of_tokens, j, token[j]);
                    }

                    char *dst_file_name = NULL;

                    storj_api->bucket_name = token[1];

                    if ((token[2] == NULL) || 
                        (strcmp(token[2], ".") == 0x00))
                    {
                        printf("******* EXECUTE UPLOAD**S** CMD HERE \n");
                        storj_upload_files(storj_api);
                    }
                    else
                    {
                        printf("[%s][%d] storj://<bucket-name>; storj://<bucket-name>/ storj://<bucket-name>/. !!!!!\n", __FUNCTION__, __LINE__);
                        goto end_program;
                    }
                } 
                else
                {
                    printf("[%s][%d] Valid dst filename missing !!!!!\n", __FUNCTION__, __LINE__);
                    goto end_program;
                }

            }
        }
        else if (strcmp(command, "upload-file") == 0)
        {
            /* get the corresponding bucket id from the bucket name */
            storj_api->bucket_name = argv[command_index + 1];
            storj_api->file_name = argv[command_index + 2];
            storj_api->dst_file = NULL;

            if (!storj_api->bucket_name || !storj_api->file_name)
            {
                printf("Missing arguments: <bucket-name> <path>\n");
                status = 1;
                goto end_program;
            }
           
            storj_upload_file(storj_api); 
        }
        else if (strcmp(command, "upload-files") == 0)
        {
            /* get the corresponding bucket id from the bucket name */
            storj_api->bucket_name = argv[command_index + 1];
            storj_api->file_path = argv[command_index + 2];
            storj_api->dst_file = NULL;

            if (!storj_api->bucket_name || !storj_api->file_name)
            {
                printf("Missing arguments: <bucket-name> <path>\n");
                status = 1;
                goto end_program;
            }

            storj_upload_files(storj_api);
        }
        else if (strcmp(command, "download-files") == 0)
        {
            /* get the corresponding bucket id from the bucket name */
            storj_api->bucket_name = argv[command_index + 1];
            storj_api->file_path = argv[command_index + 2];

            if (!storj_api->bucket_name || !storj_api->file_name)
            {
                printf("Missing arguments: <bucket-name> <path>\n");
                status = 1;
                goto end_program;
            }

            storj_download_files(storj_api);
        }
        else if (strcmp(command, "list-files") == 0)
        {
            /* get the corresponding bucket id from the bucket name */
            storj_api->bucket_name = argv[command_index + 1];

            if (!storj_api->bucket_name)
            {
                printf("Missing argument: <bucket-name>\n");
                status = 1;
                goto end_program;
            }
            storj_list_files(storj_api);
        } else if (strcmp(command, "add-bucket") == 0) {
            char *bucket_name = argv[command_index + 1];

            if (!bucket_name) {
                printf("Missing first argument: <bucket-name>\n");
                status = 1;
                goto end_program;
            }

            storj_bridge_create_bucket(env, bucket_name,
                                       NULL, create_bucket_callback);
        } else if (strcmp(command, "remove-bucket") == 0) {
            char *bucket_id = argv[command_index + 1];

            if (!bucket_id) {
                printf("Missing first argument: <bucket-id>\n");
                status = 1;
                goto end_program;
            }

            storj_api->bucket_name = argv[command_index + 1];
            storj_remove_bucket(storj_api);
        }
        else if ((strcmp(command, "remove-file") == 0) || (strcmp(command, "rm") == 0))
        {
            storj_api->bucket_name = argv[command_index + 1];
            storj_api->file_name = argv[command_index + 2];

            if (!storj_api->bucket_name|| !storj_api->file_name) 
            {
                printf("Missing arguments, expected: <bucket-name> <file-name>\n");
                status = 1;
                goto end_program;
            }

            storj_remove_file(storj_api);
        }
        else if ((strcmp(command, "list-buckets") == 0) || (strcmp(command, "ls") == 0x00))
        {
            if (argv[command_index + 1] != NULL)
            {
                /* bucket-name , used to list files */
                storj_api->bucket_name = argv[command_index + 1];

                storj_list_files(storj_api);
            }
            else
            {
                storj_bridge_get_buckets(env, NULL, get_buckets_callback);
            }
        }
        else if (strcmp(command, "get-bucket-id") == 0)
        {
            storj_api->bucket_name = argv[command_index + 1];
            storj_get_bucket_id(storj_api);
        }
        else if ((strcmp(command, "list-mirrors") == 0) || (strcmp(command, "lm") == 0))
        {
            storj_api->bucket_name = argv[command_index + 1];
            storj_api->file_name = argv[command_index + 2];

            if (!storj_api->bucket_name|| !storj_api->file_name) 
            {
                printf("Missing arguments, expected: <bucket-name> <file-name>\n");
                status = 1;
                goto end_program;
            }

            storj_list_mirrors(storj_api);
        }
        else if (strcmp(command, "test-cli") == 0)
        {
            time_t rawtime;
            char buffer [255];

            time (&rawtime);
            sprintf(buffer,"/tmp/STORJ_%s" ,ctime(&rawtime) );

            // Lets convert space to _ in
            char *p = buffer;
            for (; *p; ++p)
            {
                if (*p == ' ')
                    *p = '_';
                else if (*p == '\n')
                    *p = '\0';
            }

            printf("%s\n",buffer);
            //fopen(buffer,"w");
        }
        else
        {
            printf("'%s' is not a storj command. See 'storj --help'\n\n",
                   command);
            status = 1;
            goto end_program;
        }
    }

    // run all queued events
    if (uv_run(env->loop, UV_RUN_DEFAULT)) {
        uv_loop_close(env->loop);

        // cleanup
        storj_destroy_env(env);

        status = 1;
        goto end_program;
    }

end_program:
    if (env) {
        storj_destroy_env(env);
    }
    if (user) {
        free(user);
    }
    if (pass) {
        free(pass);
    }
    if (mnemonic) {
        free(mnemonic);
    }
    if(cli_state){
        free(cli_state);
    }

    return status;
}

/* cli cmd queue processing function */
static void queue_next_cli_cmd(cli_state_t *cli_state)
{
    void *handle = cli_state->handle;

    if (((strcmp("list-files"  , cli_state->curr_cmd_req) == 0x00)||
        ((strcmp("download-file" , cli_state->curr_cmd_req) == 0x00))) &&
        ((strcmp("list-files-1", cli_state->next_cmd_req) == 0x00)||
         (strcmp("download-file-1", cli_state->next_cmd_req)==0x00)))
    {
        if(strcmp("list-files-1" , cli_state->next_cmd_req) == 0x00)
        {
            storj_bridge_list_files(cli_state->env, cli_state->bucket_id, cli_state, list_files_callback);
        }

        if(strcmp("download-file-1" , cli_state->next_cmd_req) == 0x00)
        {
            //FILE *file = fopen("/home/kishore/libstorj/src/dwnld_list.txt", "r");
            FILE *file = fopen("dwnld_list.txt", "r");
            if (file != NULL)
            {
                char line[256][256];
                char *temp;
                char temp_path[1024];
                int i = 0x00;
                char *token[10];
                int tk_idx= 0x00;
                memset(token, 0x00, sizeof(token));
                memset(temp_path, 0x00, sizeof(temp_path));
                memset(line, 0x00, sizeof(line));
                while((fgets(line[i],sizeof(line), file)!= NULL)) /* read a line from a file */
                {
                    temp = strrchr(line[i], '\n');
                    if(temp) *temp = '\0';
                    temp = line[i];
                    i++;
                    if (i >= cli_state->curr_up_file)
                    {
                        break;
                    }
                }

                /* start tokenizing */
                token[0] = strtok(temp, ":");
                while (token[tk_idx] != NULL)
                {
                    tk_idx++;
                    token[tk_idx] = strtok(NULL, ":");
                }

                if(cli_state->curr_up_file <= cli_state->total_files)
                {
                    cli_state->file_id = token[0];
                    strcpy(temp_path, cli_state->file_path);
                    strcat(temp_path, token[1]);
                    fprintf(stdout,"*****[%d:%d] downloading file: %s *****\n",
                            cli_state->curr_up_file, cli_state->total_files, temp_path);
                    cli_state->curr_up_file++;
                    download_file(cli_state->env, cli_state->bucket_id, cli_state->file_id, temp_path, cli_state);
                }
                else
                {
                    fprintf(stdout,"***** done downloading files  *****\n");
                    fclose(file);
                    exit(0);
                }
            }
            else
            {
                download_file(cli_state->env, cli_state->bucket_id, cli_state->file_id, cli_state->file_path,cli_state);
            }

        }
    }
    else if ((strcmp("upload-file"  , cli_state->curr_cmd_req) == 0x00) &&
             (strcmp("upload-file-1", cli_state->next_cmd_req) == 0x00))
    {
        FILE *file = fopen(cli_state->file_name, "r");
        if (file != NULL)
        {
            char line[256][256];
            char *temp;
            int i = 0x00;
            memset(line, 0x00, sizeof(line));
            while((fgets(line[i],sizeof(line), file)!= NULL)) /* read a line from a file */
            {
                temp = strrchr(line[i], '\n');
                if(temp) *temp = '\0';
                cli_state->file_path = line[i];
                i++;
                printf("[%s][%d] [index = %d] target file name = %s\n", __FUNCTION__, __LINE__, i, line[i-1]);
                if(i >= cli_state->curr_up_file)
                  break;
            }
            if(cli_state->curr_up_file <= cli_state->total_files)
            {
                fprintf(stdout,"*****uploading file: %s *****\n",line[i-1]); //print the file contents on stdout.
                upload_file(cli_state->env, cli_state->bucket_id, cli_state->file_path, cli_state);
                cli_state->curr_up_file++;
            }
            else
            {
                fprintf(stdout,"***** done uploading files  *****\n");
                fclose(file);
                exit(0);
            }
        }
        else
        {
            /* handle single file upload from the command line */
            upload_file(cli_state->env, cli_state->bucket_id, cli_state->file_path, cli_state);
        }
    }
}

static int cli_upload_file(char *path, char *bucket_name, cli_state_t *cli_state)
{
    int num_of_tokens = 0x00;
    char *token[10];

    memset(token, 0x00, sizeof(token));

    int file_exist_status = file_exists(path);
    const char *file_name = NULL;
    char cwd[1024];
    char *upload_list = cwd;
    memset(upload_list, 0x00, sizeof(cwd));
    switch(file_exist_status)
    {
        case CLI_UNKNOWN_FILE_ATTR:
        case CLI_NO_SUCH_FILE_OR_DIR:
            printf("[%s][%d] file path = %s\n", __FUNCTION__, __LINE__, path);
            printf("Invalid filename \n");
        break;

        case CLI_VALID_REGULAR_FILE:
            file_name = get_filename_separator(path);

            /* token[0]-> storj:; token[1]->bucket_name; token[2]->upload_file_name */
            num_of_tokens = validate_cmd_tokenize(bucket_name, token);
            cli_state->total_files  = 0x00;
            cli_state->curr_up_file = 0x00;
            switch (num_of_tokens)
            {
                case 0x03:  /* local filename and upload filename are valid names */
                    if ((strcmp(file_name, token[2]) == 0x00) ||
                        (strcmp(token[2], ".") == 0x00))
                    {
                        cli_state->curr_cmd_req = "upload-file";
                        cli_state->bucket_name = token[1];
                        cli_state->file_path = path;
                        if(!cli_state->bucket_id)
                        {
                            storj_bridge_get_buckets(cli_state->env, cli_state, get_bucket_id_callback);
                        }
                    }
                    else
                    {
                        printf("Invalid upload target filename - ");
                        printf("Use same filename as source or '.' or blank \n");
                        return -1;
                    }
                break;

                case 0x02:  /* missing upload filename */
                    if (token[2] == NULL)
                    {
                        cli_state->curr_cmd_req = "upload-file";
                        cli_state->bucket_name = token[1];
                        cli_state->file_path = path;
                        printf("[%d] target file name = %s\n", __LINE__, file_name);
                        if(!cli_state->bucket_id)
                        {
                            storj_bridge_get_buckets(cli_state->env, cli_state, get_bucket_id_callback);
                        }
                    }
                    break;
                case 0x01:
                case 0x00:
                default:
                    printf("[%s] Invalid command ... token[2]=%s \n", __FUNCTION__, token[2]);
                    return -1;
                break;
            }
        break;

        case CLI_VALID_DIR:
            if ((upload_list = getenv("PWD")) != NULL)
            {
                fprintf(stdout, "Current working dir: %s\n", upload_list);
                strcat(upload_list, "/output.txt");
                fprintf(stdout, "Current working dir: %s\n", upload_list);
                if(file_exists(upload_list) == CLI_VALID_REGULAR_FILE)
                {
                    printf("KSA:[%s][%d] Upload file list exists \n", __FUNCTION__, __LINE__);
                }
            }
            else
            {
                perror("getenv() error");
                return -1;
            }

            printf("KSA[%s][%d] upload file : %s\n", __FUNCTION__, __LINE__,  upload_list);
            if(file_exists(upload_list) == CLI_VALID_REGULAR_FILE)
            {
                /* start reading one file at a time and upload the files */
                FILE *file = fopen ( upload_list, "r" );

                if (file != NULL)
                {
                    char line [256][256];
                    char *temp;
                    int i = 0x00;
                    memset(line, 0x00, sizeof(line));
                    cli_state->file_name =  upload_list;
                    printf("KSA[%s][%d] upload file : %s\n", __FUNCTION__, __LINE__,  upload_list);
                    printf("[%s][%d] upload file name = %s\n", __FUNCTION__, __LINE__, cli_state->file_name);
                    /* read a line from a file */
                    while(fgets(line[i],sizeof(line), file)!= NULL)
                    {
                        i++;
                    }
                    cli_state->total_files = i;
                    if(cli_state->total_files > 0x00)
                    {
                        cli_state->curr_up_file = 0x01;
                    }
                    else
                    {
                        cli_state->curr_up_file = 0x00;
                    }
                    printf("[%s][%d] total upload files = %d\n", __FUNCTION__, __LINE__, cli_state->total_files);
                    printf("[%s][%d] upload cur up file# = %d\n", __FUNCTION__, __LINE__, cli_state->curr_up_file);
                    fclose(file);
                }
                else
                {
                    /* print the error message on stderr. */
                    perror(upload_list);
                }
            }

            num_of_tokens = validate_cmd_tokenize(bucket_name, token);
            printf("KSA:[%s] num of tokens = %d \n", __FUNCTION__, num_of_tokens);
            for(int j = 0x00; j < num_of_tokens; j++)
            {
                printf("KSA:[%s] token[%d] = %s\n", __FUNCTION__, j,token[j]);
            }

            cli_state->curr_cmd_req = "upload-file";
            cli_state->bucket_name = token[1];
            printf("[%s][%d] bucket id = %s\n", __FUNCTION__, __LINE__, cli_state->bucket_id);
            if(!cli_state->bucket_id)
            {
                storj_bridge_get_buckets(cli_state->env, cli_state, get_bucket_id_callback);
            }
        break;

        default:
        break;
    }/* switch - case */

    return 0;
}

static int cli_download_file(char *path, char *bucket_name, cli_state_t *cli_state)
{
    /* download-file command */
    char *file_name = NULL;
    int num_of_tokens = 0x00;
    char *token[10];
    int ret_status = 0x00;

    memset(token, 0x00, sizeof(token));

    /* token[0]-> storj:; token[1]->bucket_name; token[2]->upload_file_name */
    num_of_tokens = validate_cmd_tokenize(bucket_name, token);

    cli_state->curr_cmd_req = "download-file";
    cli_state->bucket_name = token[1];
    cli_state->file_name = token[2];
    cli_state->file_path = path;
    if (!cli_state->bucket_name || !cli_state->file_name || !cli_state->file_path)
    {
        printf("Missing arguments: storj cp [-rR] storj://<bucket-name>/<file-name> <local_download_path>\n");
        ret_status = -1;
    }
    else
    {
        if (strcmp(cli_state->file_name, "*") == 0x00)
        {
            if (check_file_path(cli_state->file_path) == CLI_VALID_DIR)
            {
                ret_status = storj_bridge_get_buckets(cli_state->env, cli_state, get_bucket_id_callback);
            }
            else
            {
                printf("storj;// cp target '%s' is not a directory\n", cli_state->file_path);
                ret_status = -1;
            }
        }
        else
        {
            ret_status = storj_bridge_get_buckets(cli_state->env, cli_state, get_bucket_id_callback);
        }
    }
    return ret_status;
}
