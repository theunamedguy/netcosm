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

#pragma once

#include "globals.h"

#include "room.h"

/* Objects belong to an object class. Objects define their object
 * class through the class name, which is converted to a class ID
 * internally.
 */

struct object_t;

typedef struct child_data user_t; // see server.h for the definition

struct obj_class_t {
    const char *class_name;

    /* write an object's user data to disk */
    void (*hook_serialize)(int fd, struct object_t*);

     /* read an object's user data */
    void (*hook_deserialize)(int fd, struct object_t*);

    /* called when an object is taken;
     * true = can take
     * false = can't take
     * no function (NULL) = can take */
    bool (*hook_take)(struct object_t*, user_t *user);

    /* called when dropping an object;
     * true: can drop
     * false: can't drop
     * NULL: can drop
     */
    bool (*hook_drop)(struct object_t*, user_t *user);
    void* (*hook_clone)(void*); /* clone the user data pointer */
    void (*hook_destroy)(struct object_t*);
    const char* (*hook_desc)(struct object_t*, user_t*);
};

struct object_t {
    char *name; /* no articles: "a", "an", "the", needs to be free()able */

    struct obj_class_t *class;

    bool list;

    void *userdata;
};

/* returns a new object of class 'c' */
struct object_t *obj_new(const char *c);

/* serialize an object */
void obj_write(int fd, struct object_t *obj);

/* deserialize an object */
struct object_t *obj_read(int fd);

/* make a duplicate of an object
 * used for "moving" an object */
struct object_t *obj_dup(struct object_t *obj);

void obj_free(void*);

/* shut down the obj_* module */
void obj_shutdown(void);