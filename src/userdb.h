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

#include "auth.h"
#include "room.h"

/*** functions for the master process ONLY ***/

typedef enum priv_t { PRIV_NONE = -1, PRIV_USER = 0, PRIV_ADMIN = 1337 } priv_t;

struct userdata_t {
    char username[MAX_NAME_LEN + 1];

    char salt[SALT_LEN + 1];

    /* in lowercase hex + NULL terminator */
    char passhash[AUTH_HASHLEN * 2 + 1];

    priv_t priv;

    //room_id room;
    time_t last_login;

    void *objects; /* multihash of object names -> objects */

    /* for use by world module */
    void *userdata;
};

/* call before using anything else */
void userdb_init(const char *dbfile);

/* looks up a username in the DB, returns NULL upon failure */
/* changes made to the returned structure will persist */
struct userdata_t *userdb_lookup(const char *username);

bool userdb_remove(const char *username);

/* is it empty? */
size_t userdb_size(void);

/*
 * adds an entry to the DB
 * if it already exists, OVERWRITE
 */
bool userdb_add(struct userdata_t*);

void userdb_shutdown(void);

/* save the DB to disk */
bool userdb_write(const char*);

/* *save should be set to NULL on the first run */
struct userdata_t *userdb_iterate(void **save);

bool userdb_add_obj(const char *username, struct object_t *obj);
bool userdb_del_obj(const char *username, const char *obj_name);
bool userdb_del_obj_by_ptr(const char *username, struct object_t *obj);

/*** child-only functions ***/
struct userdata_t *userdb_request_lookup(const char *name);
bool userdb_request_add(struct userdata_t *data);
bool userdb_request_remove(const char *name);
void userdb_dump(void);
