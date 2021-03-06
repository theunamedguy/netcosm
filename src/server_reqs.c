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

#include "hash.h"
#include "multimap.h"
#include "server.h"
#include "server_reqs.h"
#include "userdb.h"
#include "world.h"

/* sends a single packet to a child, mostly reliable */

/* splits REQ_BCASTMSG message into multiple packets if data length
 * exceeds MSG_MAX, however, other requests will not be split and will
 * cause a failed assertion */

static void send_packet(struct child_data *child, unsigned char cmd,
                        const void *data, size_t datalen)
{
    assert(datalen < MSG_MAX || cmd == REQ_BCASTMSG);
    unsigned char pkt[MSG_MAX];
    pkt[0] = cmd;

    if(cmd == REQ_BCASTMSG && (data?datalen:0) + 1 > MSG_MAX)
    {
        /* split long messages */
        const char *ptr = data, *stop = (const char*)data + datalen;
        while(ptr < stop)
        {
            send_packet(child, cmd, ptr, MIN(stop - ptr, MSG_MAX - 1));
            ptr += MSG_MAX - 1;
        }
        return;
    }

    if(data && datalen)
        memcpy(pkt + 1, data, datalen);
tryagain:
    if(write(child->outpipe[1], pkt, (data?datalen:0) + 1) < 0)
    {
        /* write can fail, so we try again */
        if(errno == EAGAIN)
            goto tryagain;
    }
}

void child_toggle_rawmode(struct child_data *child, void (*cb)(user_t*, char *data, size_t len))
{
    if(!are_child)
    {
        send_packet(child, REQ_RAWMODE, NULL, 0);
        /* this pointer also indicates whether raw mode is on */
        if(!child->raw_mode_cb)
            child->raw_mode_cb = cb;
        else
            child->raw_mode_cb = NULL;
    }
}

void __attribute__((format(printf,2,3))) send_msg(struct child_data *child, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *buf;
    int len = vasprintf(&buf, fmt, ap);
    va_end(ap);
    send_packet(child, REQ_BCASTMSG, buf, len);
    free(buf);
}

static void req_pass_msg(unsigned char *data, size_t datalen,
                         struct child_data *sender, struct child_data *child)
{
    (void) sender;

    send_packet(child, REQ_BCASTMSG, data, datalen);
    if(child->pid != sender->pid)
        send_packet(child, REQ_ALLDONE, NULL, 0);
}

static void req_send_clientinfo(unsigned char *data, size_t datalen,
                                struct child_data *sender, struct child_data *child)
{
    (void) data;
    (void) datalen;
    char buf[128];
    const char *state[] = {
        "INIT",
        "LOGIN SCREEN",
        "CHECKING CREDENTIALS",
        "LOGGED IN AS USER",
        "LOGGED IN AS ADMIN",
        "ACCESS DENIED",
    };

    if(child->user)
        snprintf(buf, sizeof(buf), "Client %s PID %d [%s] USER %s",
                 inet_ntoa(child->addr), child->pid, state[child->state], child->user);
    else
        snprintf(buf, sizeof(buf), "Client %s PID %d [%s]",
                 inet_ntoa(child->addr), child->pid, state[child->state]);

    if(sender->pid == child->pid)
        strncat(buf, " [YOU]\n", sizeof(buf) - strlen(buf) - 1);
    else
        strncat(buf, "\n", sizeof(buf) - strlen(buf) - 1);

    send_packet(sender, REQ_BCASTMSG, buf, strlen(buf));
}

static void req_change_state(unsigned char *data, size_t datalen,
                             struct child_data *sender, struct child_data *child)
{
    (void) data; (void) datalen; (void) child; (void) sender;
    if(datalen == sizeof(sender->state))
        sender->state = *((int*)data);
}

static void req_change_user(unsigned char *data, size_t datalen,
                            struct child_data *sender, struct child_data *child)
{
    (void) data; (void) datalen; (void) child; (void) sender;
    if(sender->user)
        free(sender->user);
    sender->user = strdup((char*)data);
}

//void req_hang(unsigned char *data, size_t datalen,
//              struct child_data *sender, struct child_data *child)
//{
//    while(1);
//}

static void req_kick_client(unsigned char *data, size_t datalen,
                            struct child_data *sender, struct child_data *child)
{
    /* format is | PID | Message | */
    (void) data; (void) datalen; (void) child; (void) sender;
    if(datalen >= sizeof(pid_t))
    {
        pid_t kicked_pid = *((pid_t*)data);
        if(kicked_pid == child->pid)
        {
            send_packet(child, REQ_KICK, data + sizeof(pid_t), datalen - sizeof(pid_t));
            send_msg(sender, "Success.\n");
        }
    }
}

static void req_wait(unsigned char *data, size_t datalen, struct child_data *sender)
{
    (void) data; (void) datalen; (void) sender;
    sleep(10);
}

static void req_send_desc(unsigned char *data, size_t datalen, struct child_data *sender)
{
    (void) data; (void) datalen; (void) sender;
    struct room_t *room = room_get(sender->room);
    send_packet(sender, REQ_BCASTMSG, (void*)room->data.desc, strlen(room->data.desc));

    send_packet(sender, REQ_PRINTNEWLINE, NULL, 0);

    /* list objects */
    char buf[MSG_MAX];
    buf[0] = 0;
    void *save = NULL;
    room_id id = sender->room;
    while(1)
    {
        size_t n_objs;
        const struct multimap_list *iter = room_obj_iterate(id, &save, &n_objs);
        id = ROOM_NONE;
        if(!iter)
            break;

        const char *name = iter->key;
        struct object_t *obj = iter->val;

        buf[0] = '\0';

        if(!obj->hidden)
        {
            if(!strcmp(name, obj->name))
            {
                if(n_objs == 1)
                {
                    char *article = (is_vowel(name[0])?"an":"a");
                    strlcat(buf, "There is ", sizeof(buf));
                    if(obj->default_article)
                    {
                        strlcat(buf, article, sizeof(buf));
                        strlcat(buf, " ", sizeof(buf));
                    }
                    strlcat(buf, name, sizeof(buf));
                    strlcat(buf, " here.\n", sizeof(buf));
                }
                else
                {
                    strlcat(buf, "There are ", sizeof(buf));
                    char n[32];
                    snprintf(n, sizeof(n), "%zu ", n_objs);
                    strlcat(buf, n, sizeof(buf));
                    strlcat(buf, name, sizeof(buf));
                    strlcat(buf, "s here.\n", sizeof(buf));
                }
            }

            send_msg(sender, "%s", buf);
        }
    }
}

static void req_send_roomname(unsigned char *data, size_t datalen, struct child_data *sender)
{
    (void) data; (void) datalen; (void) sender;
    struct room_t *room = room_get(sender->room);
    if(room->data.name)
    {
        send_packet(sender, REQ_BCASTMSG, room->data.name, strlen(room->data.name));

        send_packet(sender, REQ_PRINTNEWLINE, NULL, 0);
    }
}

static void child_set_room(struct child_data *child, room_id id)
{
    child->room = id;
    room_user_add(id, child);
}

static void req_set_room(unsigned char *data, size_t datalen, struct child_data *sender)
{
    (void) data; (void) datalen; (void) sender;
    room_id id = *((room_id*)data);

    child_set_room(sender, id);
}

static void req_move_room(unsigned char *data, size_t datalen, struct child_data *sender)
{
    (void) data; (void) datalen; (void) sender;

    int status = 0;

    enum direction_t dir = *((enum direction_t*)data);
    struct room_t *current = room_get(sender->room);

    /* TODO: bounds checking on `dir' */
    room_id new = current->adjacent[dir];

    if(new == ROOM_NONE)
    {
        send_msg(sender, "You cannot go that way.\n");
    }
    else
    {
        struct room_t *new_room = room_get(new);

        if((!new_room->data.hook_enter ||
            (new_room->data.hook_enter && new_room->data.hook_enter(new, sender))) &&
           (!current->data.hook_leave ||
            (current->data.hook_leave && current->data.hook_leave(sender->room, sender))))
        {
            room_user_del(sender->room, sender);

            child_set_room(sender, new);
            status = 1;
        }
    }

    send_packet(sender, REQ_MOVE, &status, sizeof(status));
}

static void req_send_user(unsigned char *data, size_t datalen, struct child_data *sender)
{
    if(datalen)
    {
        struct userdata_t *user = userdb_lookup((char*)data);

        if(user)
        {
            send_packet(sender, REQ_GETUSERDATA, user, sizeof(*user));
            return;
        }
    }
}

static void req_del_user(unsigned char *data, size_t datalen, struct child_data *sender)
{
    bool success = false;
    if(datalen)
    {
        success = userdb_remove((char*)data);
    }
    send_packet(sender, REQ_DELUSERDATA, &success, sizeof(success));
}

static void req_add_user(unsigned char *data, size_t datalen, struct child_data *sender)
{
    bool success = false;
    if(datalen == sizeof(struct userdata_t))
    {
        success = userdb_add((struct userdata_t*)data);
    }
    send_packet(sender, REQ_ADDUSERDATA, &success, sizeof(success));
}

static void req_send_geninfo(unsigned char *data, size_t datalen, struct child_data *sender)
{
    (void) data;
    (void) datalen;
    send_msg(sender, "Total clients: %d\n", num_clients);
}

static void req_kick_always(unsigned char *data, size_t datalen,
                            struct child_data *sender, struct child_data *child)
{
    (void) sender;
    send_packet(child, REQ_KICK, data, datalen);
}

static int print_objlist(struct child_data *sender, const struct multimap_list *list, int idx, size_t n_objs)
{
    if(list)
    {
        while(list)
        {
            struct object_t *obj = list->val;

            const char *desc = obj->class->hook_desc(obj, sender);
            if(n_objs > 1)
                send_msg(sender, "%d) %s\n", idx++, desc);
            else
                send_msg(sender, "%s\n", desc);

            list = list->next;
        }
    }
    return idx;
}

static void req_look_at(unsigned char *data, size_t datalen, struct child_data *sender)
{
    (void) datalen;
    size_t n_objs = 0, tmp;

    const struct multimap_list *room_list = room_obj_get_size(sender->room, (const char*)data, &n_objs);
    const struct multimap_list *inv_list = multimap_lookup(userdb_lookup(sender->user)->objects, data, &tmp);

    int idx = 1; // index of the object
    n_objs += tmp;

    if(room_list)
    {
        send_msg(sender, "In room:\n");
        idx = print_objlist(sender, room_list, idx, n_objs);
    }
    if(inv_list)
    {
        send_msg(sender, "In inventory:\n");
        print_objlist(sender, inv_list, idx, n_objs);
    }

    if(!room_list && !inv_list)
        send_msg(sender, "I don't know what that is.\n");
}

static void req_take(unsigned char *data, size_t datalen, struct child_data *sender)
{
    (void) datalen;
    const struct multimap_list *iter = room_obj_get(sender->room, (const char*)data), *next;
    if(iter)
    {
        while(iter)
        {
            next = iter->next;
            struct object_t *obj = iter->val;
            if(obj)
            {
                if(obj->class->hook_take && !obj->class->hook_take(obj, sender))
                {
                    send_msg(sender, "You can't take that.\n");
                    iter = next;
                    continue;
                }

                userdb_add_obj(sender->user, obj);
                room_obj_del_by_ptr(sender->room, obj);

                send_msg(sender, "Taken.\n");
            }
            else
                break;
            iter = next;
        }

        server_save_state(false);
    }
    else
    {
        send_msg(sender, "I don't know what that is.\n");
    }
}

static void req_inventory(unsigned char *data, size_t datalen, struct child_data *sender)
{
    (void) datalen;
    (void) data;

    void *ptr = userdb_lookup(sender->user)->objects, *save;

    send_msg(sender, "You currently have:\n");

    while(1)
    {
        size_t n_objs;
        const struct multimap_list *iter = multimap_iterate(ptr, &save, &n_objs);

        if(!iter)
            break;

        ptr = NULL;

        char buf[MSG_MAX];
        buf[0] = '\0';

        const char *name = iter->key;
        struct object_t *obj = iter->val;
        if(!strcmp(name, obj->name))
        {
            format_noun(buf, sizeof(buf), name, n_objs, obj->default_article, true);
            strlcat(buf, "\n", sizeof(buf));

            send_packet(sender, REQ_BCASTMSG, buf, strlen(buf));
        }
    }
    if(ptr)
        send_msg(sender, "Nothing!\n");
}

static void req_drop(unsigned char *data, size_t datalen, struct child_data *sender)
{
    (void) datalen;
    (void) data;

    struct userdata_t *user = userdb_lookup(sender->user);

    if(!user)
        return;

    size_t n_objs;
    const struct multimap_list *iter = multimap_lookup(user->objects, (const char*)data, &n_objs);

    if(!iter)
    {
        send_msg(sender, "You don't have that.\n");
        return;
    }

    while(iter)
    {
        const struct multimap_list *next = iter->next;
        struct object_t *obj = iter->val;

        room_obj_add(sender->room, obj_dup(obj));
        userdb_del_obj_by_ptr(sender->user, obj);

        if(obj->class->hook_drop && !obj->class->hook_drop(obj, sender))
        {
            send_msg(sender, "You cannot drop that.\n");
            userdb_add_obj(sender->user, obj_dup(obj));
            room_obj_del_by_ptr(sender->room, obj);
        }
        else
            send_msg(sender, "Dropped.\n");

        iter = next;
    }

    server_save_state(false);
}

static void req_listusers(unsigned char *data, size_t datalen, struct child_data *sender)
{
    (void) data;
    (void) datalen;

    void *save = NULL;
    while(1)
    {
        struct userdata_t *user = userdb_iterate(&save);
        if(!user)
            break;

        send_msg(sender, "%s: priv: %d last: %s", user->username,
                 user->priv,
                 ctime(&user->last_login));
    }
}

static void req_execverb(unsigned char *data, size_t datalen, struct child_data *sender)
{
    (void) datalen;

    /* if the child is in raw mode, pass the data to the world module */
    if(sender->raw_mode_cb)
    {
        sender->raw_mode_cb(sender, (char*)data, datalen);
        return;
    }

    /* first look for a room-local verb */
    char *save;
    char *tok = strtok_r((char*)data, " \t", &save);

    all_lower(tok);

    void *local_map = room_verb_map(sender->room);
    struct verb_t *verb = hash_lookup(local_map, tok);
    if(verb)
        goto exec_verb;

    /* now search the global map */
    void *global_map = world_verb_map();
    verb = hash_lookup(global_map, tok);

    if(verb)
        goto exec_verb;

    send_msg(sender, "I don't know what that means.\n");
    return;

    char *args;
exec_verb:
    args = strtok_r(NULL, "", &save);
    verb->class->hook_exec(verb, args, sender);
}

static const struct child_request {
    unsigned char code;

    bool havedata;

    enum { CHILD_NONE, CHILD_SENDER, CHILD_ALL_BUT_SENDER, CHILD_ALL } which;

    /* sender_pipe is the pipe to the sender of the request */
    /* data points to bogus if havedata = false */
    void (*handle_child)(unsigned char *data, size_t len,
                         struct child_data *sender, struct child_data *child);

    void (*finalize)(unsigned char *data, size_t len, struct child_data *sender);
} requests[] = {
    {  REQ_NOP,             false,  CHILD_NONE,            NULL,                 NULL,               },
    {  REQ_BCASTMSG,        true,   CHILD_ALL,             req_pass_msg,         NULL,               },
    {  REQ_CHANGESTATE,     true,   CHILD_SENDER,          req_change_state,     NULL,               },
    {  REQ_CHANGEUSER,      true,   CHILD_SENDER,          req_change_user,      NULL,               },
    {  REQ_KICK,            true,   CHILD_ALL,             req_kick_client,      NULL,               },
    {  REQ_KICKALL,         true,   CHILD_ALL_BUT_SENDER,  req_kick_always,      NULL,               },
    {  REQ_LISTCLIENTS,     false,  CHILD_ALL,             req_send_clientinfo,  req_send_geninfo,   },
    {  REQ_SETROOM,         true,   CHILD_NONE,            NULL,                 req_set_room,       },
    {  REQ_MOVE,            true,   CHILD_NONE,            NULL,                 req_move_room,      },
    {  REQ_GETUSERDATA,     true,   CHILD_NONE,            NULL,                 req_send_user,      },
    {  REQ_DELUSERDATA,     true,   CHILD_NONE,            NULL,                 req_del_user,       },
    {  REQ_ADDUSERDATA,     true,   CHILD_NONE,            NULL,                 req_add_user,       },
    {  REQ_LOOKAT,          true,   CHILD_NONE,            NULL,                 req_look_at,        },
    {  REQ_TAKE,            true,   CHILD_NONE,            NULL,                 req_take,           },
    {  REQ_DROP,            true,   CHILD_NONE,            NULL,                 req_drop,           },
    {  REQ_EXECVERB,        true,   CHILD_NONE,            NULL,                 req_execverb        },
    {  REQ_WAIT,            false,  CHILD_NONE,            NULL,                 req_wait,           },
    {  REQ_GETROOMDESC,     false,  CHILD_NONE,            NULL,                 req_send_desc,      },
    {  REQ_GETROOMNAME,     false,  CHILD_NONE,            NULL,                 req_send_roomname,  },
    {  REQ_PRINTINVENTORY,  false,  CHILD_NONE,            NULL,                 req_inventory,      },
    {  REQ_LISTUSERS,       false,  CHILD_NONE,            NULL,                 req_listusers       },
    //{ REQ_ROOMMSG,     true,  CHILD_ALL,            req_send_room_msg,   NULL,           },
};

static SIMP_HASH(unsigned char, uchar_hash);
static SIMP_EQUAL(unsigned char, uchar_equal);

static void *request_map = NULL;

void reqmap_init(void)
{
    request_map = hash_init(ARRAYLEN(requests), uchar_hash, uchar_equal);
    for(unsigned i = 0; i < ARRAYLEN(requests); ++i)
        hash_insert(request_map, &requests[i].code, requests + i);
}

void reqmap_free(void)
{
    if(request_map)
    {
        hash_free(request_map);
        request_map = NULL;
    }
}

/**
 * Here's how child-parent requests work
 * 1. Child writes its PID and length of request to the parent's pipe, followed
 *    by up to MSG_MAX bytes of data. If the length exceeds MSG_MAX bytes, the
 *    request will be ignored.
 * 1.1 Child spins until parent response.
 * 2. Parent handles the request.
 * 3. Parent writes its PID and length of message back to the child(ren).
 * 4. Parent signals child(ren) with SIGRTMIN
 * 5. Child(ren) handle parent's message.
 * 6. Child(ren) send the parent SIGRTMIN+1 to acknowledge receipt of message.
 * 7. Parent spins until the needed number of signals is reached.
 */

static unsigned char packet[MSG_MAX + 1];

bool handle_child_req(int in_fd)
{
    ssize_t packet_len = read(in_fd, packet, MSG_MAX);

    if((size_t)packet_len < sizeof(pid_t) + 1)
    {
        /* the pipe is probably broken (i.e. disconnect), so we don't
         * try to send a reply */
        return false;
    }

    struct child_data *sender = NULL;

    pid_t sender_pid;
    memcpy(&sender_pid, packet, sizeof(pid_t));

    sender = hash_lookup(child_map, &sender_pid);

    if(!sender)
    {
        debugf("WARNING: got data from unknown PID, ignoring.\n");
        goto fail;
    }

    unsigned char cmd = packet[sizeof(pid_t)];

    unsigned char *data = packet + sizeof(pid_t) + 1;
    size_t datalen = packet_len - sizeof(pid_t) - 1;

    struct child_request *req = hash_lookup(request_map, &cmd);

    //debugf("Child %d sends request %d\n", sender_pid, cmd);

    if(!req)
    {
        debugf("Unknown request.\n");
        goto fail;
    }

    switch(req->which)
    {
    case CHILD_SENDER:
    case CHILD_ALL:
        req->handle_child(data, datalen, sender, sender);
        if(req->which == CHILD_SENDER)
            goto finish;
        break;
    case CHILD_NONE:
        goto finish;
    default:
        break;
    }

    struct child_data *child = NULL;
    void *ptr = child_map, *save;

    do {
        pid_t *key;
        child = hash_iterate(ptr, &save, (void**)&key);
        ptr = NULL;
        if(!child)
            break;
        if(child->pid == sender->pid)
            continue;

        switch(req->which)
        {
        case CHILD_ALL:
        case CHILD_ALL_BUT_SENDER:
            req->handle_child(data, datalen, sender, child);
            break;
        default:
            break;
        }
    } while(1);

finish:

    if(req && req->finalize)
        req->finalize(data, datalen, sender);

    /* fall through */
fail:

    if(sender)
        send_packet(sender, REQ_ALLDONE, NULL, 0);

    return true;
}
