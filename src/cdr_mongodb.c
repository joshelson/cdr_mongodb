/*
 * cdr_mongodb.c
 *
 * Copyright 2010 Flavio [FlaPer87] Percoco Premoli <flaper87@flaper87.org>
 *
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License v2 as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*! \file
 *
 * \brief Mongodb based asterisk CDR
 *
 * \author lavio [FlaPer87] Percoco Premoli <flaper87@flaper87.org>
 *
 * \arg See also \ref AstCDR
 * \ingroup cdr_drivers
 */

#include <asterisk.h>

#include <sys/types.h>
#include <asterisk/config.h>
#include <asterisk/options.h>
#include <asterisk/channel.h>
#include <asterisk/cdr.h>
#include <asterisk/module.h>
#include <asterisk/logger.h>
#include <asterisk/cli.h>
#include <asterisk/strings.h>
#include <asterisk/linkedlists.h>
#include <asterisk/threadstorage.h>

#include <stdio.h>
#include <string.h>

#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include <mongo.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

static char *desc = "MongoDB CDR Backend";
static char *name = "mongodb";
static char *config = "cdr_mongodb.conf";

/* Allow spaces or commas as delimiters for customfields */
static char delimiters[] = " ,";

static struct ast_str *hostname = NULL, *dbname = NULL, *dbuser = NULL, *password = NULL, *dbcollection = NULL, *customfields = NULL;

static int dbport = 0;
static int connected = 0;
static int records = 0;
static int totalrecords = 0;
static int hasCustomFields = 0; 

AST_MUTEX_DEFINE_STATIC(mongodb_lock);

struct unload_string {
	AST_LIST_ENTRY(unload_string) entry;
	struct ast_str *str;
};

static AST_LIST_HEAD_STATIC(unload_strings, unload_string);

static int internal_bson_append_date(bson * bb, const char *name, struct timeval when)
{
	char tmp[128] = "";
	struct ast_tm tm;

	if (ast_tvzero(when)) {
		bson_append_string(bb, name, "");
		return 0;
	}

	ast_localtime(&when, &tm, NULL);
	ast_strftime(tmp, sizeof(tmp), "%Y-%m-%d %T", &tm);

	bson_append_string(bb, name, tmp);
	return 0;
}

static int _unload_module(int reload)
{
	ast_cdr_unregister(name);
	
	if (hostname) {
		ast_free(hostname);	
	}
	if (dbname) {
		ast_free(dbname);	
	}
	if (dbuser) {
		ast_free(dbuser);	
	}
	if (password) {
		ast_free(password);	
	}
	if (dbcollection) {
		ast_free(dbcollection);	
	}
	if (customfields) {
		ast_free(customfields);	
	}
	
	return 0;
}

static int mongodb_log(struct ast_cdr *cdr)
{
	const char * ns;
	char *result = NULL;
	char buf[257], *value;
	mongo conn[1];
	mongo_error_t status;	

	ast_debug(1, "mongodb: Starting mongodb_log.\n");
	ast_debug(1, "mongodb: Connecting to host == %s\n", ast_str_buffer(hostname));

	ast_debug(1, "mongodb: Building mongodb ns.\n");
	strcpy(&ns, ast_str_buffer(dbname));
	ast_debug(1, "mongodb: ns == %s.\n", &ns);
	strcat(&ns, ".");
	strcat(&ns, ast_str_buffer(dbcollection));
	ast_debug(1, "mongodb: ns == %s.\n", &ns);

	status = mongo_connect( conn , ast_str_buffer(hostname), dbport );
	
	if ( status != MONGO_OK ){
		connected = 0;
		records = 0;
		ast_log(LOG_ERROR, "Method: mongodb_log, unable to connect to MongoDB.\n" );

			switch ( conn->err ) {
	            	case MONGO_CONN_SUCCESS:      ast_debug(1, "connection succeeded\n" ); break;
	              	case MONGO_CONN_NO_SOCKET:    ast_debug(1, "no socket\n" ); return -1;
	              	case MONGO_CONN_FAIL:         ast_debug(1, "connection failed\n" ); return -1;
	             	case MONGO_CONN_ADDR_FAIL:    ast_debug(1, "error while calling getaddrinfo\n" ); return -1;
	              	case MONGO_CONN_NOT_MASTER:   ast_debug(1, "not master\n" ); return -1;
	              	case MONGO_CONN_BAD_SET_NAME: ast_debug(1, "given name doesn't match replica set\n" ); return -1;
	              	case MONGO_CONN_NO_PRIMARY:   ast_debug(1, "can't find primary in replica set\n" ); return -1;
			}

		return -1;
	}

	ast_debug(1, "mongodb: Locking mongodb_lock.\n");
	ast_mutex_lock(&mongodb_lock);

	ast_debug(1, "mongodb: Got connection lock, Preparing record.\n");

	bson bb[1];
	mongo_cursor * cursor;

	/* Init the bson buffer */
	bson_init( bb );
	bson_append_new_oid( &bb, "_id" );

	bson_append_string( &bb , "accountcode",  cdr->accountcode);

	bson_append_string( &bb , "src",  cdr->src);

	bson_append_string( &bb, "dst" , cdr->dst );

	bson_append_string( &bb, "dcontext" , cdr->dcontext );

	bson_append_string( &bb, "clid" , cdr->clid );

	bson_append_string( &bb, "channel" , cdr->channel );

	bson_append_string( &bb, "dstchannel" , cdr->dstchannel );

	bson_append_string( &bb, "lastapp" , cdr->lastapp );

	bson_append_string( &bb, "lastdata" , cdr->lastdata );

	internal_bson_append_date( &bb, "start" , cdr->start );

	internal_bson_append_date( &bb, "answer" , cdr->answer );

	internal_bson_append_date( &bb, "end" , cdr->end );

	bson_append_int( &bb, "duration" , cdr->duration );

	bson_append_int( &bb, "billsec" , cdr->billsec );

	bson_append_string( &bb, "disposition" , ast_cdr_disp2str(cdr->disposition) );

	bson_append_string( &bb, "amaflags" , ast_cdr_flags2str(cdr->amaflags) );

	bson_append_string( &bb, "uniqueid" , cdr->uniqueid );

	bson_append_string( &bb, "userfield" , cdr->userfield );

	/* Read in special values! */
	if ( hasCustomFields = 1 ) {
		/* Split custom fields string into array */
		result = strtok( ast_str_buffer(customfields), delimiters );
		while( result != NULL ) {
			ast_cdr_getvar( cdr, result, &value, buf, sizeof(buf), 0, 0 );
			if ( !ast_strlen_zero( value ) ) {
				ast_debug(1, "mongodb: Custom CDR entry %s for %s\n", result, value);
				bson_append_string ( &bb, result, value );
			}
		result = strtok( NULL, delimiters );
		}
	}

	bson_finish(bb);

	ast_debug(1, "mongodb: Inserting a CDR record.\n");
	mongo_insert( conn , &ns, bb );
	bson_destroy( &bb );
	mongo_destroy( conn );

	connected = 1;
	records++;
	totalrecords++;

	ast_debug(1, "Unlocking mongodb_lock.\n");
	ast_mutex_unlock( &mongodb_lock );
	return 0;
}

static int load_config_string(struct ast_config *cfg, const char *category, const char *variable, struct ast_str **field, const char *def)
{
	struct unload_string *us;
	const char *tmp;

	if (!(us = ast_calloc(1, sizeof(*us)))) {
		return -1;
	}

	if (!(*field = ast_str_create(16))) {
		ast_free(us);
		return -1;
	}

	tmp = ast_variable_retrieve(cfg, category, variable);

	ast_str_set(field, 0, "%s", tmp ? tmp : def);

	us->str = *field;

	AST_LIST_LOCK(&unload_strings);
	AST_LIST_INSERT_HEAD(&unload_strings, us, entry);
	AST_LIST_UNLOCK(&unload_strings);

	return 0;
}

static char *handle_cli_cdr_mongodb_status(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "cdr mongodb status";
		e->usage =
			"Usage: cdr mongodb status\n"
			"       Shows current connection status for cdr_mongodb\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 3) {
		return CLI_SHOWUSAGE;
	}

	if (connected) {
		char status[256], status2[100] = "";
		if (dbport) {
			snprintf(status, sizeof(status), "Connected to %s@%s, port %d", ast_str_buffer(dbname), ast_str_buffer(hostname), dbport);
		} else {
			snprintf(status, sizeof(status), "Connected to %s@%s", ast_str_buffer(dbname), ast_str_buffer(hostname));
		}

		if (!ast_strlen_zero(ast_str_buffer(dbuser))) {
			snprintf(status2, sizeof(status2), " with username %s", ast_str_buffer(dbuser));
		}

		if (ast_str_strlen(dbcollection)) {
			snprintf(status2, sizeof(status2), " using collection %s", ast_str_buffer(dbcollection));
		}

		if (records == totalrecords) {
			ast_cli(a->fd, "  Wrote %d records since last restart.\n", totalrecords);
		} else {
			ast_cli(a->fd, "  Wrote %d records since last restart and %d records since last reconnect.\n", totalrecords, records);
		}
	} else {
		ast_cli(a->fd, "Not currently connected to a MongoDB server.\n");
	}

	return CLI_SUCCESS;
}

static struct ast_cli_entry cdr_mongodb_status_cli[] = {
	AST_CLI_DEFINE(handle_cli_cdr_mongodb_status, "Show connection status of cdr_mongodb"),
};

static int load_config_number(struct ast_config *cfg, const char *category, const char *variable, int *field, int def)
{
	const char *tmp;

	tmp = ast_variable_retrieve(cfg, category, variable);

	if (!tmp || sscanf(tmp, "%d", field) < 1) {
		*field = def;
	}

	return 0;
}

static int _load_module(int reload)
{
	int res;
	struct ast_config *cfg;
	struct ast_variable *var;
	struct ast_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	const char * host;
	
	mongo conn[1];
	mongo_error_t status;
	bson bb;
	mongo_cursor * cursor;

	ast_debug(1, "Starting mongodb module load.\n");
	ast_debug(1, "Loading mongodb Config.\n");

	if (!(cfg = ast_config_load(config, config_flags)) || cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_WARNING, "Unable to load config for mongodb CDR's: %s\n", config);
		return AST_MODULE_LOAD_SUCCESS;
	} else if (cfg == CONFIG_STATUS_FILEUNCHANGED) {
		return AST_MODULE_LOAD_SUCCESS;
	}

	if (reload) {
		_unload_module(1);
	}

	ast_debug(1, "Browsing mongodb Global.\n");
	var = ast_variable_browse(cfg, "global");
	if (!var) {
		return AST_MODULE_LOAD_SUCCESS;
	}

	res = 0;

	res |= load_config_string(cfg, "global", "hostname", &hostname, "localhost");
	res |= load_config_string(cfg, "global", "dbname", &dbname, "astriskcdrdb");
	res |= load_config_string(cfg, "global", "user", &dbuser, "");
	res |= load_config_string(cfg, "global", "collection", &dbcollection, "cdr");
	res |= load_config_string(cfg, "global", "password", &password, "");
	res |= load_config_number(cfg, "global", "port", &dbport, 27017);
	res |= load_config_string(cfg, "global", "customfields", &customfields, "");


	if (res < 0) {
		return AST_MODULE_LOAD_FAILURE;
	}

	ast_debug(1, "Got hostname of %s\n", ast_str_buffer(hostname));
	ast_debug(1, "Got port of %d\n", dbport);
	ast_debug(1, "Got user of %s\n", ast_str_buffer(dbuser));
	ast_debug(1, "Got dbname of %s\n", ast_str_buffer(dbname));
	ast_debug(1, "Got dbcollection of %s\n", ast_str_buffer(dbcollection));
	ast_debug(1, "Got password of %s\n", ast_str_buffer(password));
	
	/* See if we should be looking for custom fields */
	if (!ast_strlen_zero( ast_str_buffer(customfields) ) ) {
		ast_debug(1, "Got custom field list of %s\n", ast_str_buffer(customfields));
		hasCustomFields = 1;
	}

	ast_copy_string(&host, ast_str_buffer(hostname), 255);

	status = mongo_connect( conn , &host, dbport );	

	if( status != MONGO_OK ) {
		switch ( conn->err ) {
		      	case MONGO_CONN_SUCCESS:      ast_debug(1, "connection succeeded\n" ); break;
              	case MONGO_CONN_NO_SOCKET:    ast_debug(1, "no socket\n" ); res = -1;
              	case MONGO_CONN_FAIL:         ast_debug(1, "connection failed\n" ); res = -1;
             	case MONGO_CONN_ADDR_FAIL:    ast_debug(1, "error while calling getaddrinfo\n" ); res = -1;
              	case MONGO_CONN_NOT_MASTER:   ast_debug(1, "not master\n" ); res = -1;
              	case MONGO_CONN_BAD_SET_NAME: ast_debug(1, "given name doesn't match replica set\n" ); res = -1;
              	case MONGO_CONN_NO_PRIMARY:   ast_debug(1, "can't find primary in replica set\n" ); res = -1;
	      }
	}
	else {
		ast_log(LOG_NOTICE, "Method: _load_module, MongoDB connected successfully\n");
		connected = 1;
		mongo_destroy( conn );
	}

	ast_config_destroy(cfg);

	res = ast_cdr_register(name, desc, mongodb_log);
	if (res) {
		ast_log(LOG_ERROR, "Unable to register MongoDB CDR handling\n");
	} else {
		res = ast_cli_register_multiple(cdr_mongodb_status_cli, sizeof(cdr_mongodb_status_cli) / sizeof(struct ast_cli_entry));
	}

	return res;
}


static int load_module(void)
{
	return _load_module(0);
}

static int unload_module(void)
{
	return _unload_module(0);
}

static int reload(void)
{
	return _load_module(1);
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "MongoDB CDR Backend",
	.load = load_module,
	.unload = unload_module,
	.reload = reload,
);
