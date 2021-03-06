/*
 *   NetCosm - a MUD server
 *   Copyright (C) 2016 Franklin Wei
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "globals.h"

#include "client.h"
#include "hash.h"
#include "server.h"
#include "server_reqs.h"
#include "userdb.h"
#include "util.h"
#include "world.h"

#define DEFAULT_PORT 1234
#define BACKLOG 512

/* global data */
bool are_child = false;
void *child_map = NULL;

/* assume int is atomic */
volatile int num_clients = 0;

/* local data */
static uint16_t port = DEFAULT_PORT;

static int server_socket;

/* for debugging: */
static char *world_module = "build/worlds/dunnet.so";
static char *module_handle = NULL;

/* save after every X changes to the world state */
#define SAVE_INTERVAL 10

/* saves game state periodically */
void server_save_state(bool force)
{
    if(!are_child)
    {
        static int n = 0;
        n = (n + 1) % SAVE_INTERVAL;
        if(!n || force)
        {
            world_save(WORLDFILE);
            userdb_write(USERFILE);
        }
    }
}

static void free_child_data(void *ptr)
{
    struct child_data *child = ptr;
    if(child->user)
    {
        free(child->user);
        child->user = NULL;
    }
    if(child->io_watcher)
    {
        ev_io_stop(EV_DEFAULT_ child->io_watcher);

        free(child->io_watcher);
        child->io_watcher = NULL;
    }
    free(ptr);
}

static void handle_disconnects(void)
{
    int saved_errno = errno;

    pid_t pid;
    while((pid = waitpid(-1, NULL, WNOHANG)) > 0)
    {
        struct child_data *child = hash_lookup(child_map, &pid);

        debugf("Client disconnect.\n");

        room_user_del(child->room, child);

        --num_clients;

        hash_remove(child_map, &pid);
    }

    errno = saved_errno;
}

volatile sig_atomic_t reap_children = 0;

static void sigchld_handler(int sig)
{
    (void) sig;
    reap_children = 1;
}

static void handle_client(int fd, struct sockaddr_in *addr,
                          int nclients, int to, int from)
{
    client_main(fd, addr, nclients, to, from);
}

static void __attribute__((noreturn)) server_shutdown(void)
{
    if(!are_child)
        debugf("Shutdown server.\n");
    else
        debugf("Shutdown worker.\n");

    if(shutdown(server_socket, SHUT_RDWR) > 0)
        error("shutdown");

    close(server_socket);

    /* save state */
    if(!are_child)
        server_save_state(true);

    /* shut down modules */
    client_shutdown();
    obj_shutdown();
    reqmap_free();
    userdb_shutdown();
    verb_shutdown();
    world_free();

    /* free internal data structures */
    hash_free(child_map);
    child_map = NULL;

    extern void *dir_map;
    hash_free(dir_map);
    dir_map = NULL;

    extern char *current_user;
    if(current_user)
        free(current_user);

    if(module_handle)
        dlclose(module_handle);

    /* shut down libev */
    ev_default_destroy();

    _exit(0);
}

static void __attribute__((noreturn)) sigint_handler(int s)
{
    (void) s;
    exit(0);
}

static bool autoconfig = false;
static const char *autouser, *autopass;

static void check_userfile(void)
{
    if(access(USERFILE, F_OK) < 0 || userdb_size() == 0)
    {
        if(!autoconfig)
            first_run_setup();
        else
            auth_user_add(autouser, autopass, PRIV_ADMIN);
        userdb_write(USERFILE);
    }

    if(access(USERFILE, R_OK | W_OK) < 0)
        error("cannot access "USERFILE);
}

static void load_worldfile(void)
{
    /* load the world module */

    module_handle = dlopen(world_module, RTLD_NOW);
    if(!module_handle)
        error("cannot load world module `%s' (%s)", world_module, dlerror());

    /* load symbols */
    {
        size_t *ptr;

        netcosm_verb_classes = dlsym(module_handle, "netcosm_verb_classes");
        ptr = dlsym(module_handle, "netcosm_verb_classes_sz");
        netcosm_verb_classes_sz = *ptr;

        netcosm_obj_classes = dlsym(module_handle, "netcosm_obj_classes");
        ptr = dlsym(module_handle, "netcosm_obj_classes_sz");
        netcosm_obj_classes_sz = *ptr;

        netcosm_world = dlsym(module_handle, "netcosm_world");
        ptr = dlsym(module_handle, "netcosm_world_sz");
        netcosm_world_sz = *ptr;
    }
    {
        char **ptr = dlsym(module_handle, "netcosm_world_name");
        netcosm_world_name = *ptr;
    }
    {
        void *ptr;
        ptr = dlsym(module_handle, "netcosm_world_simulation_cb");
        netcosm_world_simulation_cb = ptr;
    }
    {
        unsigned *ptr;
        ptr = dlsym(module_handle, "netcosm_world_simulation_interval");
        if(ptr)
            netcosm_world_simulation_interval = *ptr;
        else
        {
            netcosm_world_simulation_interval = 0;
            if(netcosm_world_simulation_cb)
                error("have simulation callback, but no interval specified");
        }
    }

    netcosm_write_userdata_cb = dlsym(module_handle, "netcosm_write_userdata_cb");
    netcosm_read_userdata_cb = dlsym(module_handle, "netcosm_read_userdata_cb");

    if(access(WORLDFILE, F_OK) < 0)
    {
        world_init(netcosm_world, netcosm_world_sz, netcosm_world_name);

        world_save(WORLDFILE);
    }
    else if(access(WORLDFILE, R_OK | W_OK) < 0)
        error("cannot access "WORLDFILE);
    else
        if(!world_load(WORLDFILE, netcosm_world, netcosm_world_sz, netcosm_world_name))
            error("Failed to load world from disk.\nTry removing "WORLDFILE".");
}

static int server_bind(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if(sock<0)
        error("socket");

    int tmp = 1;

    if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &tmp, sizeof tmp) < 0)
        error("setsockopt");

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if(bind(sock, (struct sockaddr*) &addr, sizeof addr) < 0)
        error("bind");

    if(listen(sock, BACKLOG) < 0)
        error("listen");

    return sock;
}

static void childreq_cb(EV_P_ ev_io *w, int revents)
{
    (void) EV_A;
    (void) w;
    /* data from a child's pipe */
    if(revents & EV_READ)
    {
        if(!handle_child_req(w->fd))
        {
            handle_disconnects();
        }
    }
    if(reap_children)
        handle_disconnects();
}

static void new_connection_cb(EV_P_ ev_io *w, int revents)
{
    (void) EV_A;
    (void) w;
    (void) revents;
    struct sockaddr_in client;
    socklen_t client_len = sizeof(client);
    int new_sock = accept(server_socket, (struct sockaddr*) &client, &client_len);
    if(new_sock < 0)
        error("accept");

    ++num_clients;

    int readpipe[2]; /* child->parent */
    int outpipe [2]; /* parent->child */

    /* try several methods to create a packet pipe between the child and master: */

    /* first try creating a pipe in "packet mode": see pipe(2) */
    if(pipe2(readpipe, O_DIRECT) < 0)
    {
        /* then try a SOCK_SEQPACKET socket pair: see unix(7) */
        if(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, readpipe) < 0)
        {
            /* if that failed, try a SOCK_DGRAM socket as a last resort */
            if(socketpair(AF_UNIX, SOCK_DGRAM, 0, readpipe) < 0)
                error("couldn't create child-master communication pipe");
            else
                debugf("WARNING: Using a SOCK_DGRAM socket pair for IPC, performance may be degraded.\n");
        }
    }

    if(pipe2(outpipe, O_DIRECT) < 0)
    {
        if(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, outpipe) < 0)
        {
            if(socketpair(AF_UNIX, SOCK_DGRAM, 0, outpipe) < 0)
                error("error creating IPC pipe");
        }
    }

    pid_t pid = fork();
    if(pid < 0)
        error("fork");

    if(!pid)
    {
        /* child */
        are_child = true;

        /* close unused file descriptors */
        close(readpipe[0]);
        close(outpipe[1]);
        close(server_socket);

        /* shut down modules */
        obj_shutdown();
        reqmap_free();
        userdb_shutdown();
        verb_shutdown();
        world_free();

        /* free our data structures */
        hash_free(child_map);
        child_map = NULL;

        if(module_handle)
            dlclose(module_handle);
        module_handle = NULL;

        /* shut down libev */
        ev_default_destroy();

        server_socket = new_sock;

        handle_client(new_sock, &client, num_clients, readpipe[1], outpipe[0]);

        exit(0);
    }
    else
    {
        /* parent */
        close(readpipe[1]);
        close(outpipe[0]);
        close(new_sock);

        /* add the child to the child map */
        struct child_data *new = calloc(1, sizeof(struct child_data));
        memcpy(new->outpipe, outpipe, sizeof(outpipe));
        memcpy(new->readpipe, readpipe, sizeof(readpipe));
        new->addr = client.sin_addr;
        new->pid = pid;
        new->state = STATE_INIT;
        new->user = NULL;

        ev_io *new_io_watcher = calloc(1, sizeof(ev_io));
        ev_io_init(new_io_watcher, childreq_cb, new->readpipe[0], EV_READ);
        ev_set_priority(new_io_watcher, EV_MINPRI);
        ev_io_start(EV_A_ new_io_watcher);
        new->io_watcher = new_io_watcher;

        pid_t *pidbuf = malloc(sizeof(pid_t));
        *pidbuf = pid;

        hash_insert(child_map, pidbuf, new);
    }
}

static void init_signals(void)
{
    struct sigaction sa;

    /* SIGINT and SIGTERM cause graceful shutdown */
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGINT);
    sa.sa_handler = sigint_handler;
    sa.sa_flags = SA_RESTART;
    if(sigaction(SIGINT, &sa, NULL) < 0)
        error("sigaction");
    if(sigaction(SIGTERM, &sa, NULL) < 0)
        error("sigaction");

    /* ignore SIGPIPE */
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    if(sigaction(SIGPIPE, &sa, NULL) < 0)
        error("sigaction");

    /* libev's default SIGCHLD handler exhibits some really strange
     * behavior, which we don't like, so we use our own */
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGCHLD);
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART;
    if(sigaction(SIGCHLD, &sa, NULL) < 0)
        error("sigaction");
}

static void __attribute__((noreturn)) print_help(char *argv[])
{
    debugf("Usage: %s [OPTION]...\n", argv[0]);
    debugf("NetCosm MUD server\n");
    debugf("\n");
    debugf(" -a USER PASS\tautomatic setup with USER/PASS\n");
    debugf(" -d PREFIX\tcreate and change to PREFIX before writing data files\n");
    debugf(" -h, -?\t\tshow this help\n");
    debugf(" -p PORT\tlisten on PORT\n");
    debugf(" -w MODULE\tuse a different world module\n");
    exit(0);
}

static char *data_prefix = NULL;

static void parse_args(int argc, char *argv[])
{
    for(int i = 1; i < argc; ++i)
    {
        if(argv[i][0] == '-')
        {
            if(strlen(argv[i]) > 1)
            {
                char c = argv[i][1];
            retry:
                switch(c)
                {
                case 'h': /* help */
                case '?':
                    print_help(argv);
                case 'a': /* automatic first-run config */
                    autoconfig = true;
                    if(i + 2 > argc)
                        print_help(argv);
                    autouser = argv[++i];
                    autopass = argv[++i];
                    break;
                case 'd': /* set data prefix */
                    if(i + 1 > argc)
                        print_help(argv);
                    data_prefix = argv[++i];
                    break;
                case 'p': /* set port */
                    if(i + 1 > argc)
                        print_help(argv);
                    port = strtol(argv[++i], NULL, 10);
                    break;
                case 'w': /* world */
                    if(i + 1 > argc)
                        print_help(argv);
                    world_module = argv[++i];
                    break;
                default:
                    c = 'h';
                    goto retry;
                }
            }
        }
        else
        {
            debugf("Unknown argument `%s'\n", argv[i]);
            exit(0);
        }
    }
}

static SIMP_HASH(pid_t, pid_hash);
static SIMP_EQUAL(pid_t, pid_equal);

static void check_libs(void)
{
    debugf("*** NetCosm %s (libev %d.%d, %s) ***\n",
           NETCOSM_VERSION, EV_VERSION_MAJOR, EV_VERSION_MINOR, OPENSSL_VERSION_TEXT);
    assert(ev_version_major() == EV_VERSION_MAJOR &&
           ev_version_minor() >= EV_VERSION_MINOR);
}

int server_main(int argc, char *argv[])
{
    check_libs();

    parse_args(argc, argv);

    /* load default if none specified */
    if(!world_module)
    {
        error("no world module specified");
    }

    /* this must be done before any world module data is used */
    load_worldfile();

    if(data_prefix)
    {
        mkdir(data_prefix, 0700);
        if(chdir(data_prefix) < 0)
        {
            debugf("Cannot access data prefix.\n");
            exit(0);
        }
    }

    userdb_init(USERFILE);

    /* also performs first-time setup: */
    check_userfile();

    /* initialize request map */
    reqmap_init();

    /* save some time after a fork() */
    client_init();

    /* this initial size is set very low to make iteration faster */
    child_map = hash_init(16, pid_hash, pid_equal);
    hash_setfreedata_cb(child_map, free_child_data);
    hash_setfreekey_cb(child_map, free);

    debugf("Listening on port %d.\n", port);

    server_socket = server_bind();

    struct ev_loop *loop = ev_default_loop(0);

    /* we initialize signals after creating the default event loop
     * because libev grabs SIGCHLD in the process */
    init_signals();

    ev_io server_watcher;
    ev_io_init(&server_watcher, new_connection_cb, server_socket, EV_READ);
    ev_set_priority(&server_watcher, EV_MAXPRI);

    ev_io_start(EV_A_ &server_watcher);

    atexit(server_shutdown);

    /* everything's ready, hand it over to libev */
    ev_loop(loop, 0);

    /* should never get here */
    error("FIXME: unexpected termination");
}
