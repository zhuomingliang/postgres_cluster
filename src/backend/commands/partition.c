/*-------------------------------------------------------------------------
 *
 * partitions.c
 *	  partitioning utilities
 *
 * Copyright (c) 2015-2016, Postgres Professional
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "catalog/dependency.h"
#include "catalog/pg_type.h"
#include "commands/partition.h"
#include "commands/pathman_wrapper.h"
#include "commands/tablecmds.h"
#include "commands/event_trigger.h"
#include "executor/spi.h"
#include "nodes/value.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"
#include "utils/lsyscache.h"
#include "parser/parse_node.h"
#include "access/htup_details.h"


static void create_hash_partitions(CreateStmt *stmt, Oid relid, const char* attname);
static void create_range_partitions(CreateStmt *stmt, Oid relid, const char *attname);
static Node *cookPartitionKeyValue(Oid relid, const char *raw, Node *raw_value);
static char *RangeVarGetString(const RangeVar *rangevar);
static Oid RangeVarGetNamespaceId(const RangeVar *rangevar);
static List* makeNameListFromRangeVar(const RangeVar *rangevar);

#define equalstr(a, b)	\
	(((a) != NULL && (b) != NULL) ? (strcmp(a, b) == 0) : (a) == (b))


void
create_partitions(CreateStmt *stmt, Oid relid)
{
	PartitionInfo *pinfo = (PartitionInfo *) stmt->partition_info;
	Value  *attname = (Value *) linitial(((ColumnRef *) pinfo->key)->fields);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect using SPI");

	switch (pinfo->partition_type)
	{
		case P_HASH:
			{
				create_hash_partitions(stmt, relid, strVal(attname));
				break;
			}
		case P_RANGE:
			{
				create_range_partitions(stmt, relid, strVal(attname));
				break;
			}
	}

	SPI_finish(); /* close SPI connection */
}


static void
create_hash_partitions(CreateStmt *stmt, Oid relid, const char* attname)
{
	PartitionInfo *pinfo = (PartitionInfo *) stmt->partition_info;
	char		 **relnames = NULL;

	if (pinfo->partitions_count > 0)
	{
		ListCell   *lc;
		int			i = 0;

		/* Convert RangeVars into cstrings */
		relnames = palloc(sizeof(char *) * pinfo->partitions_count);
		foreach(lc, pinfo->partitions)
		{
			RangePartitionInfo *p = (RangePartitionInfo *) lfirst(lc);

			relnames[i] = RangeVarGetString(p->relation);
			i++;
		}
	}

	pm_create_hash_partitions(relid,
							  attname,
							  pinfo->partitions_count,
							  relnames,
							  NULL);

	pfree(relnames);
}


/*
 * Extracts partitioning parameters from statement, creates partitioned table
 * and all partitions via pg_pathman's wrapper functions
 */
static void
create_range_partitions(CreateStmt *stmt, Oid relid, const char *attname)
{
	ListCell	   *lc;
	Datum			last_bound = (Datum) 0;
	bool			last_bound_is_null = true;
	PartitionInfo  *pinfo = (PartitionInfo *) stmt->partition_info;

	/* partitioning key */
	AttrNumber	attnum = get_attnum(relid, attname);
	Oid			atttype;
	int32		atttypmod;

	/* parameters */
	Datum		interval_datum = (Datum) 0;
	Oid			interval_type;

	/* for parsing interval */
	Const	   *interval_const;
	ParseState *pstate = make_parsestate(NULL);

	if (!attnum)
		elog(ERROR,
			 "Unknown attribute '%s'",
			 attname);
	atttype = get_atttype(relid, attnum);
	atttypmod = get_atttypmod(relid, attnum);

	/* Convert interval to Const node */
	if (pinfo->interval != NULL)
	{
		if (IsA(pinfo->interval, A_Const))
		{
			A_Const    *con = (A_Const *) pinfo->interval;
			Value	   *val = &con->val;

			interval_const = make_const(pstate, val, con->location);
		}
		else
			elog(ERROR, "Constant interval value is expected");

		/* If attribute is of type DATE or TIMESTAMP then convert interval to Interval type */
		if (atttype == DATEOID || atttype == TIMESTAMPOID || atttype == TIMESTAMPTZOID)
		{
			char	   *interval_literal;

			/* We should get an UNKNOWN type here */
			if (interval_const->consttype != UNKNOWNOID)
				elog(ERROR, "Expected a literal as an interval value");

			/* Get a text representation of the interval */
			interval_literal = DatumGetCString(interval_const->constvalue);
			interval_datum = DirectFunctionCall3(interval_in,
												 CStringGetDatum(interval_literal),
												 ObjectIdGetDatum(InvalidOid),
												 Int32GetDatum(-1));
			interval_type = INTERVALOID;
		}
		else
		{
			interval_datum = interval_const->constvalue;
			interval_type = interval_const->consttype;
		}
	}
	else /* If interval is not set  */
	{
		if (atttype == DATEOID || atttype == TIMESTAMPOID || atttype == TIMESTAMPTZOID)
			interval_type = INTERVALOID;
		else
			interval_type = atttype;
	}

	/* Invoke pg_pathman's wrapper */
	pm_create_range_partitions(relid,
							   attname,
							   atttype,
							   interval_datum,
							   interval_type,
							   pinfo->interval == NULL);

	/* Add partitions */
	foreach(lc, pinfo->partitions)
	{
		RangePartitionInfo *p = (RangePartitionInfo *) lfirst(lc);
		Node *orig = (Node *) p->upper_bound;
		Node *bound_expr;

		/* Transform raw expression */
		bound_expr = cookDefault(pstate, orig, atttype, atttypmod, (char *) attname);

		if (!IsA(bound_expr, Const))
			elog(ERROR, "Constant expected");

		pm_add_range_partition(relid,
							((Const *) bound_expr)->consttype,
							p->relation ? p->relation->relname : NULL,
							last_bound,
							((Const *) bound_expr)->constvalue,
							last_bound_is_null,
							false,
							p->tablespace);
		last_bound = ((Const *) bound_expr)->constvalue;
		last_bound_is_null = false;
	}
}


static Node *
cookPartitionKeyValue(Oid relid, const char *attname, Node *raw_value)
{
	Node	   *cookie;
	AttrNumber	attnum = get_attnum(relid, attname);
	Oid			atttype = get_atttype(relid, attnum);
	int32		atttypmod = get_atttypmod(relid, attnum);
	ParseState *pstate = make_parsestate(NULL);

	Assert(atttype != InvalidOid);

	/* TODO: write own cook- function for partition key */
	cookie = cookDefault(pstate,
						 raw_value,
						 atttype,
						 atttypmod,
						 (char *) attname);

	return cookie;
}


void
add_range_partition(Oid parent, RangePartitionInfo *rpinfo)
{
	char	   *attname;
	AttrNumber	attnum;
	Oid			atttype;
	Datum		lower,
				upper;
	Node	   *bound;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect using SPI");

	/* Partitioning attribute parameters */
	attname = pm_get_partition_key(parent);
	attnum = get_attnum(parent, attname);
	atttype = get_atttype(parent, attnum);

	bound = cookPartitionKeyValue(parent, attname, (Node *) rpinfo->upper_bound);

	if (!IsA(bound, Const))
		elog(ERROR, "Constant expected");

	pm_get_part_range(parent, -1, atttype, &lower, &upper);
	pm_add_range_partition(parent,
						   atttype,
						   rpinfo->relation ? rpinfo->relation->relname : NULL,
						   upper,
						   ((Const *) bound)->constvalue,
						   false,
						   false,
						   rpinfo->tablespace);

	SPI_finish();
}


void
merge_range_partitions(List *partitions)
{
	Oid			p1_relid,
				p2_relid;

	/*
	 * There could be two or three partitions: two partitions are input ones
	 * and the last is the output one. If the last is absent then partitions
	 * are merged into the first one
	 */
	Assert(list_length(partitions) >= 2);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect using SPI");

	/* Convert rangevars to relids */
	p1_relid = RangeVarGetRelid(linitial(partitions), NoLock, false);
	p2_relid = RangeVarGetRelid(lsecond(partitions), NoLock, false);

	/* Merge */
	pm_merge_range_partitions(p1_relid,
							  p2_relid);

	/* Handle INTO clause (if there is one) */
	if (list_length(partitions) > 2)
	{
		/* Last object in the list is the output partition */
		RangePartitionInfo *output = (RangePartitionInfo *) lthird(partitions);
		Oid		new_namespace = RangeVarGetNamespaceId(output->relation);

		/*
		 * When merging data pg_pathman copies data to the first partition.
		 * Oracle does it slightly different, it creates a new partition and
		 * merges all data there. So to simulate this behaviour we are renaming
		 * and (if needed) moving the first partition to a new tablespace
		 */
		pm_alter_partition(p1_relid,
						   output->relation->relname,
						   new_namespace,
						   output->tablespace);
	}

	SPI_finish();
}


void
split_range_partition(Oid parent,
					  AlterTableCmd *cmd)
{
	Node	   *split_value;
	RangePartitionInfo *orig, *p1, *p2;
	char	   *attname;
	Oid			partition_relid;
	char	   *p2_relname = NULL;
	char	   *p2_tablespace = NULL;

	/*
	 * partitions list should contain at least one element -- the relation
	 * we are splitting. It also may contain two other relations which contain
	 * names and tablespaces for resulting partitions
	 */
	Assert(list_length(cmd->partitions) >= 1);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect using SPI");

	attname = pm_get_partition_key(parent);

	/* Split value is stored in def attribute */
	orig = (RangePartitionInfo *) linitial(cmd->partitions);
	split_value = cookPartitionKeyValue(parent, attname, (Node *) cmd->def);

	partition_relid = RangeVarGetRelid(orig->relation, NoLock, false);

	/*
	 * When splitting partition pg_pathman leaves first partition name and
	 * tablespace unchanged and sets the second partition's name and tablespace
	 * according to parameters (or makes up a default name if it hasn't been
	 * provided). Oracle on the other hand makes up names for both partitions
	 * (or uses provided names).
	 *
	 * To simulate the same behaviour using pg_pathman we at first provide
	 * the name of second partition and then (see below) rename the first one.
	 * But we need to do this only if partition names are provided. Otherwise
	 * use standard pg_pathman's behaviour.
	 */
	if (list_length(cmd->partitions) == 3)
	{
		p1 = (RangePartitionInfo *) lsecond(cmd->partitions);
		p2 = (RangePartitionInfo *) lthird(cmd->partitions);

		p2_relname = RangeVarGetString(p2->relation);
		p2_tablespace = p2->tablespace;
	}

	pm_split_range_partition(partition_relid,
							 ((Const *) split_value)->constvalue,
							 ((Const *) split_value)->consttype,
							 p2_relname,
							 p2_tablespace);

	/* Rename the first partition if the name is provided */
	if (list_length(cmd->partitions) == 3)
	{
		Oid			new_namespace;

		/* Get new schema oid */
		new_namespace = RangeVarGetNamespaceId(p1->relation);

		/* Rename original partition or move it to another tablespace if needed */
		pm_alter_partition(partition_relid, p1->relation->relname, new_namespace, p1->tablespace);
	}

	SPI_finish();
}


/*
 * Rename partition is just a rename table statement with additional
 * checks
 */
void
rename_partition(Oid parent, AlterTableCmd *cmd)
{
	Oid		partition;

	/* TODO: Check that parent is an actual parent of partition */

	Assert(list_length(cmd->partitions) == 1);

	partition = RangeVarGetRelid((RangeVar *) linitial(cmd->partitions),
								 NoLock,
								 false);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect using SPI");

	/*
	 * We cannot just execute RenameStmt because in this case
	 * process_utility_hook won't be called. And pg_pathman relies on this
	 */
	pm_alter_partition(partition, cmd->name, InvalidOid, NULL);

	SPI_finish();
}


/*
 * Drop partition
 */
void
drop_partition(Oid parent, AlterTableCmd *cmd)
{
	DropStmt *n = makeNode(DropStmt);
	RangeVar *partition;

	/* TODO: Check that parent is an actual parent of partition */

	Assert(list_length(cmd->partitions) == 1);

	partition = (RangeVar *) linitial(cmd->partitions);

	n->removeType = OBJECT_TABLE;
	n->missing_ok = false;
	n->objects = list_make1(makeNameListFromRangeVar(partition));
	n->arguments = NIL;
	n->behavior = DROP_RESTRICT;  // default behaviour
	n->concurrent = false;

	RemoveRelations(n);
}


/*
 * Set a new tablespace for partition
 */
void
move_partition(Oid parent, AlterTableCmd *cmd)
{
	Oid				partition;

	/* TODO: Consider possibility to run SET TABLESPACE command */
	// AlterTableStmt *ts_stmt;
	// AlterTableCmd  *ts_cmd;
	// Oid				partition;

	// Assert(list_length(cmd->partitions) == 1);

	// partition = RangeVarGetRelid((RangeVar *) linitial(cmd->partitions),
	// 							 NoLock,
	// 							 false);

	// ts_cmd = makeNode(AlterTableCmd);
	// ts_cmd->subtype = AT_SetTableSpace;
	// ts_cmd->name = cmd->name;

	// ts_stmt = makeNode(AlterTableStmt);
	// ts_stmt->relation = (RangeVar *) linitial(cmd->partitions);
	// ts_stmt->cmds = list_make1(ts_cmd);
	// ts_stmt->relkind = OBJECT_TABLE;
	// ts_stmt->missing_ok = false;

	// EventTriggerAlterTableStart((Node *) ts_stmt);
	// AlterTableInternal(partition, list_make1(ts_cmd), false);
	// EventTriggerAlterTableEnd();

	Assert(list_length(cmd->partitions) == 1);

	partition = RangeVarGetRelid((RangeVar *) linitial(cmd->partitions),
								 NoLock,
								 false);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect using SPI");

	pm_alter_partition(partition, NULL, InvalidOid, cmd->name);

	SPI_finish();
}


static char *
RangeVarGetString(const RangeVar *rangevar)
{
	if (!rangevar->schemaname)
		return rangevar->relname;
	else
		return psprintf("%s.%s", rangevar->schemaname, rangevar->relname);
}


static Oid
RangeVarGetNamespaceId(const RangeVar *rangevar)
{
	Oid		namespace_id;

	if (rangevar->schemaname == NULL)
	{
		List	   *search_path = fetch_search_path(false);

		namespace_id = linitial_oid(search_path);
		list_free(search_path);
	}
	else
		namespace_id = get_namespace_oid(rangevar->schemaname, false);

	return namespace_id;
}


/*
 * This is the inverse function for makeRangeVarFromNameList()
 */
static List*
makeNameListFromRangeVar(const RangeVar *rangevar)
{
	Oid		namespace_id = RangeVarGetNamespaceId(rangevar);

	return list_make2(makeString(get_namespace_name(namespace_id)),
					  makeString(rangevar->relname));
}
