/*-------------------------------------------------------------------------
 *
 * pglogical_proto_json.c
 * 		pglogical protocol functions for json support
 *
 * Copyright (c) 2015, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  pglogical_proto_json.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"

#include "pglogical_output.h"
#include "pglogical_proto_json.h"

#include "access/sysattr.h"
#include "access/tuptoaster.h"
#include "access/xact.h"

#include "catalog/catversion.h"
#include "catalog/index.h"

#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"

#include "commands/dbcommands.h"

#include "executor/spi.h"

#include "libpq/pqformat.h"

#include "mb/pg_wchar.h"

#ifdef HAVE_REPLICATION_ORIGINS
#include "replication/origin.h"
#endif

#include "utils/builtins.h"
#include "utils/json.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"


/*
 * Write BEGIN to the output stream.
 */
void
pglogical_json_write_begin(StringInfo out, PGLogicalOutputData *data, ReorderBufferTXN *txn)
{
	appendStringInfoChar(out, '{');
	appendStringInfoString(out, "\"action\":\"B\"");
	appendStringInfo(out, ", \"has_catalog_changes\":\"%c\"",
		txn->has_catalog_changes ? 't' : 'f');
#ifdef HAVE_REPLICATION_ORIGINS
	if (txn->origin_id != InvalidRepOriginId)
		appendStringInfo(out, ", \"origin_id\":\"%u\"", txn->origin_id);
#endif
	if (!data->client_no_txinfo)
	{
		appendStringInfo(out, ", \"xid\":\"%u\"", txn->xid);
		appendStringInfo(out, ", \"first_lsn\":\"%X/%X\"",
			(uint32)(txn->first_lsn >> 32), (uint32)(txn->first_lsn));
#ifdef HAVE_REPLICATION_ORIGINS
		appendStringInfo(out, ", \"origin_lsn\":\"%X/%X\"",
			(uint32)(txn->origin_lsn >> 32), (uint32)(txn->origin_lsn));
#endif
		if (txn->commit_time != 0)
		appendStringInfo(out, ", \"commit_time\":\"%s\"",
			timestamptz_to_str(txn->commit_time));
	}
	appendStringInfoChar(out, '}');
}

/*
 * Write COMMIT to the output stream.
 */
void
pglogical_json_write_commit(StringInfo out, PGLogicalOutputData *data, ReorderBufferTXN *txn,
						XLogRecPtr commit_lsn)
{
	appendStringInfoChar(out, '{');

	if (txn->xact_action == XLOG_XACT_PREPARE)
	{
		appendStringInfoString(out, "\"action\":\"P\"");
		appendStringInfo(out, ", \"gid\":\"%s\"", txn->gid);
	}
	else if (txn->xact_action == XLOG_XACT_COMMIT_PREPARED)
	{
		appendStringInfoString(out, "\"action\":\"CP\"");
		appendStringInfo(out, ", \"gid\":\"%s\"", txn->gid);
	}
	else if (txn->xact_action == XLOG_XACT_ABORT_PREPARED)
	{
		appendStringInfoString(out, "\"action\":\"AP\"");
		appendStringInfo(out, ", \"gid\":\"%s\"", txn->gid);
	}
	else if (txn->xact_action == XLOG_XACT_COMMIT)
	{
		appendStringInfoString(out, "\"action\":\"C\"");
	}
	else
	{
		Assert(false);
	}

	if (!data->client_no_txinfo)
	{
		appendStringInfo(out, ", \"final_lsn\":\"%X/%X\"",
			(uint32)(txn->final_lsn >> 32), (uint32)(txn->final_lsn));
		appendStringInfo(out, ", \"end_lsn\":\"%X/%X\"",
			(uint32)(txn->end_lsn >> 32), (uint32)(txn->end_lsn));
	}
	appendStringInfoChar(out, '}');
}

/*
 * Write a tuple to the outputstream, in the most efficient format possible.
 */
static void
json_write_tuple(StringInfo out, Relation rel, HeapTuple tuple)
{
	TupleDesc	desc;
	Datum		tupdatum,
				json;

	desc = RelationGetDescr(rel);
	tupdatum = heap_copy_tuple_as_datum(tuple, desc);
	json = DirectFunctionCall1(row_to_json, tupdatum);

	appendStringInfoString(out, TextDatumGetCString(json));
}

/*
 * Write change.
 *
 * Generic function handling DML changes.
 */
static void
pglogical_json_write_change(StringInfo out, const char *change, Relation rel,
							HeapTuple oldtuple, HeapTuple newtuple)
{
	appendStringInfoChar(out, '{');
	appendStringInfo(out, "\"action\":\"%s\",\"relation\":[\"%s\",\"%s\"]",
					 change,
					 get_namespace_name(RelationGetNamespace(rel)),
					 RelationGetRelationName(rel));

	if (oldtuple)
	{
		appendStringInfoString(out, ",\"oldtuple\":");
		json_write_tuple(out, rel, oldtuple);
	}
	if (newtuple)
	{
		appendStringInfoString(out, ",\"newtuple\":");
		json_write_tuple(out, rel, newtuple);
	}
	appendStringInfoChar(out, '}');
}

/*
 * Write INSERT to the output stream.
 */
void
pglogical_json_write_insert(StringInfo out, PGLogicalOutputData *data,
							Relation rel, HeapTuple newtuple)
{
	pglogical_json_write_change(out, "I", rel, NULL, newtuple);
}

/*
 * Write UPDATE to the output stream.
 */
void
pglogical_json_write_update(StringInfo out, PGLogicalOutputData *data,
							Relation rel, HeapTuple oldtuple,
							HeapTuple newtuple)
{
	pglogical_json_write_change(out, "U", rel, oldtuple, newtuple);
}

/*
 * Write DELETE to the output stream.
 */
void
pglogical_json_write_delete(StringInfo out, PGLogicalOutputData *data,
							Relation rel, HeapTuple oldtuple)
{
	pglogical_json_write_change(out, "D", rel, oldtuple, NULL);
}

/*
 * The startup message should be constructed as a json object, one
 * key/value per DefElem list member.
 */
void
json_write_startup_message(StringInfo out, List *msg)
{
	ListCell *lc;
	bool first = true;

	appendStringInfoString(out, "{\"action\":\"S\", \"params\": {");
	foreach (lc, msg)
	{
		DefElem *param = (DefElem*)lfirst(lc);
		Assert(IsA(param->arg, String) && strVal(param->arg) != NULL);
		if (first)
			first = false;
		else
			appendStringInfoChar(out, ',');
		escape_json(out, param->defname);
		appendStringInfoChar(out, ':');
		escape_json(out, strVal(param->arg));
	}
	appendStringInfoString(out, "}}");
}
