/*
 * bdr.h
 *
 * BiDirectionalReplication
 *
 * Copyright (c) 2012-2013, PostgreSQL Global Development Group
 *
 * contrib/bdr/bdr.h
 */
#ifndef BDR_H
#define BDR_H

#include "access/xlogdefs.h"
#include "postmaster/bgworker.h"
#include "replication/logical.h"
#include "utils/resowner.h"
#include "storage/lock.h"

#include "libpq-fe.h"

#include "bdr_config.h"

#include "bdr_internal.h"

#include "bdr_replication_identifier.h"

#include "bdr_version.h"

/* Right now replication_name isn't used; make it easily found for later */
#define EMPTY_REPLICATION_NAME ""

/*
 * BDR_LOCALID_FORMAT is used in fallback_application_name. It's distinct from
 * BDR_NODE_ID_FORMAT in that it doesn't include the remote dboid as that may
 * not be known yet, just (sysid,tlid,dboid,replication_name) .
 *
 * Use BDR_LOCALID_FORMAT_ARGS to sub it in to format strings.
 */
#define BDR_LOCALID_FORMAT "bdr ("UINT64_FORMAT",%u,%u,%s)"

#define BDR_LOCALID_FORMAT_ARGS \
	GetSystemIdentifier(), ThisTimeLineID, MyDatabaseId, EMPTY_REPLICATION_NAME

#define BDR_INIT_REPLICA_CMD "bdr_initial_load"


/*
 * Don't include libpq here, msvc infrastructure requires linking to libpq
 * otherwise.
 */
struct pg_conn;

/* Forward declarations */
struct TupleTableSlot; /* from executor/tuptable.h */
struct EState; /* from nodes/execnodes.h */
struct ScanKeyData; /* from access/skey.h for ScanKey */
enum LockTupleMode; /* from access/heapam.h */

/*
 * Flags to indicate which fields are present in a begin record sent by the
 * output plugin.
 */
typedef enum BdrOutputBeginFlags
{
	BDR_OUTPUT_TRANSACTION_HAS_ORIGIN = 1
} BdrOutputBeginFlags;

/*
 * BDR conflict detection: type of conflict that was identified.
 *
 * Must correspond to bdr.bdr_conflict_type SQL enum and
 * bdr_conflict_type_get_datum (...)
 */
typedef enum BdrConflictType
{
	BdrConflictType_InsertInsert,
	BdrConflictType_InsertUpdate,
	BdrConflictType_UpdateUpdate,
	BdrConflictType_UpdateDelete,
	BdrConflictType_DeleteDelete,
	BdrConflictType_UnhandledTxAbort
} BdrConflictType;

/*
 * BDR conflict detection: how the conflict was resolved (if it was).
 *
 * Must correspond to bdr.bdr_conflict_resolution SQL enum and
 * bdr_conflict_resolution_get_datum(...)
 */
typedef enum BdrConflictResolution
{
	BdrConflictResolution_ConflictTriggerSkipChange,
	BdrConflictResolution_ConflictTriggerReturnedTuple,
	BdrConflictResolution_LastUpdateWins_KeepLocal,
	BdrConflictResolution_LastUpdateWins_KeepRemote,
	BdrConflictResolution_DefaultApplyChange,
	BdrConflictResolution_DefaultSkipChange,
	BdrConflictResolution_UnhandledTxAbort
} BdrConflictResolution;

typedef struct BDRConflictHandler
{
	Oid			handler_oid;
	BdrConflictType handler_type;
	uint64		timeframe;
}	BDRConflictHandler;

/*
 * This structure is for caching relation specific information, such as
 * conflict handlers.
 */
typedef struct BDRRelation
{
	/* hash key */
	Oid			reloid;

	bool		valid;

	Relation	rel;

	BDRConflictHandler *conflict_handlers;
	size_t		conflict_handlers_len;

	/* ordered list of replication sets of length num_* */
	char	  **replication_sets;
	/* -1 for no configured set */
	int			num_replication_sets;

	bool		computed_repl_valid;
	bool		computed_repl_insert;
	bool		computed_repl_update;
	bool		computed_repl_delete;
} BDRRelation;

typedef struct BDRTupleData
{
	Datum		values[MaxTupleAttributeNumber];
	bool		isnull[MaxTupleAttributeNumber];
	bool		changed[MaxTupleAttributeNumber];
} BDRTupleData;

/*
 * BdrApplyWorker describes a BDR worker connection.
 *
 * This struct is stored in an array in shared memory, so it can't have any
 * pointers.
 */
typedef struct BdrApplyWorker
{
	/*
	 * Index in bdr_connection_configs of this workers's GUCs
	 * and config info (including dbname, name, etc).
	 */
	int connection_config_idx;

	/*
	 * If not InvalidXLogRecPtr, stop replay at this point and exit.
	 *
	 * To save shmem space in apply workers, this is reset to InvalidXLogRecPtr
	 * if replay is successfully completed instead of setting a separate flag.
	 */
	XLogRecPtr replay_stop_lsn;

	/* Request that the remote forward all changes from other nodes */
	bool forward_changesets;

	/*
	 * Ensure this worker doesn't get registered a second time if there's a
	 * perdb worker restart or postmaster restart. Ideally we'd store the
	 * BackgroundWorkerHandle, but it's an opaque struct.
	 */
	bool bgw_is_registered;

	size_t perdb_worker_off;
} BdrApplyWorker;

/*
 * BDRPerdbCon describes a per-database worker, a static bgworker that manages
 * BDR for a given DB.
 */
typedef struct BdrPerdbWorker
{
	/* local database name */
	NameData dbname;

	/* number of outgoing connections from this database */
	size_t nnodes;

	size_t seq_slot;

} BdrPerdbWorker;

/*
 * Type of BDR worker in a BdrWorker struct
 */
typedef enum {
	/*
	 * This shm array slot is unused and may be allocated. Must be zero, as
	 * it's set by memset(...) during shm segment init.
	 */
	BDR_WORKER_EMPTY_SLOT = 0,
	/* This shm array slot contains data for a */
	BDR_WORKER_APPLY,
	/* This is data for a per-database worker BdrPerdbWorker */
	BDR_WORKER_PERDB,
	/*
	 * Data for a walsender output plugin. Never currently allocated but still
	 * defined for use in the bdr_worker_type global.
	 */
	BDR_WORKER_WALSENDER
} BdrWorkerType;

/*
 * BDRWorker entries describe shared memory slots that keep track of
 * all BDR worker types. A slot may contain data for a number of different
 * kinds of worker; this union makes sure each slot is the same size and
 * is easily accessed via an array.
 */
typedef struct BdrWorker
{
	/* Type of worker. Also used to determine if this shm slot is free. */
	BdrWorkerType worker_type;

	union worker_data {
		BdrApplyWorker apply_worker;
		BdrPerdbWorker perdb_worker;
	} worker_data;

} BdrWorker;

/*
 * Params for every connection in bdr.connections.
 *
 * Contains n=bdr_max_workers elements, may have NULL entries.
 */
extern BdrConnectionConfig	**bdr_connection_configs;

/* GUCs */
extern int	bdr_default_apply_delay;
extern int bdr_max_workers;
extern char *bdr_temp_dump_directory;
extern bool bdr_init_from_basedump;
extern bool bdr_log_conflicts_to_table;
extern bool bdr_conflict_logging_include_tuples;
extern bool bdr_permit_unsafe_commands;
#ifdef BUILDING_UDR
extern bool bdr_conflict_default_apply;
#endif

/*
 * Header for the shared memory segment ref'd by the BdrWorkerCtl ptr,
 * containing bdr_max_workers entries of BdrWorkerCon .
 */
typedef struct BdrWorkerControl
{
	/* Must hold this lock when writing to BdrWorkerControl members */
	LWLockId     lock;
	/* Set/unset by bdr_apply_pause()/_replay(). */
	bool		 pause_apply;
	/* Array members, of size bdr_max_workers */
	BdrWorker    slots[FLEXIBLE_ARRAY_MEMBER];
} BdrWorkerControl;

extern BdrWorkerControl *BdrWorkerCtl;

extern ResourceOwner bdr_saved_resowner;

/* DDL executor/filtering support */
extern bool in_bdr_replicate_ddl_command;

/* bdr_nodes table oid */
extern Oid	BdrNodesRelid;
extern Oid  BdrConflictHistoryRelId;

/* DDL replication support */
extern Oid	QueuedDDLCommandsRelid;
extern Oid	QueuedDropsRelid;

/* sequencer support */
extern Oid	BdrSequenceValuesRelid;
extern Oid	BdrSequenceElectionsRelid;
extern Oid	BdrVotesRelid;

extern Oid	BdrLocksRelid;
extern Oid	BdrLocksByOwnerRelid;

extern Oid  BdrReplicationSetConfigRelid;

extern Oid bdr_lookup_relid(const char *relname, Oid schema_oid);

/* apply support */
extern void bdr_process_remote_action(StringInfo s);
extern void bdr_fetch_sysid_via_node_id(RepNodeId node_id, uint64 *sysid,
										TimeLineID *tli, Oid *remote_dboid);
extern RepNodeId bdr_fetch_node_id_via_sysid(uint64 sysid, TimeLineID tli, Oid dboid);

/* Index maintenance, heap access, etc */
extern struct EState * bdr_create_rel_estate(Relation rel);
extern void UserTableUpdateIndexes(struct EState *estate,
								   struct TupleTableSlot *slot);
extern void UserTableUpdateOpenIndexes(struct EState *estate,
									   struct TupleTableSlot *slot);
extern void build_index_scan_keys(struct EState *estate,
								  struct ScanKeyData **scan_keys,
								  BDRTupleData *tup);
extern bool build_index_scan_key(struct ScanKeyData *skey, Relation rel,
								 Relation idxrel,
								 BDRTupleData *tup);
extern bool find_pkey_tuple(struct ScanKeyData *skey, BDRRelation *rel,
							Relation idxrel, struct TupleTableSlot *slot,
							bool lock, enum LockTupleMode mode);

/* conflict logging (usable in apply only) */

/*
 * Details of a conflict detected by an apply process, destined for logging
 * output and/or conflict triggers.
 *
 * Closely related to bdr.bdr_conflict_history SQL table.
 */
typedef struct BdrApplyConflict
{
	TransactionId			local_conflict_txid;
	XLogRecPtr				local_conflict_lsn;
	TimestampTz				local_conflict_time;
	const char			   *object_schema; /* unused if apply_error */
	const char			   *object_name;   /* unused if apply_error */
	uint64					remote_sysid;
	TimeLineID				remote_tli;
	Oid						remote_dboid;
	TransactionId			remote_txid;
	TimestampTz				remote_commit_time;
	XLogRecPtr				remote_commit_lsn;
	BdrConflictType			conflict_type;
	BdrConflictResolution	conflict_resolution;
	bool					local_tuple_null;
	Datum					local_tuple;    /* composite */
	TransactionId			local_tuple_xmin;
	uint64					local_tuple_origin_sysid; /* init to 0 if unknown */
	TimeLineID				local_tuple_origin_tli;
	Oid						local_tuple_origin_dboid;
	bool					remote_tuple_null;
	Datum					remote_tuple;   /* composite */
	ErrorData			   *apply_error;
} BdrApplyConflict;

extern void bdr_conflict_logging_startup(void);
extern void bdr_conflict_logging_cleanup(void);

extern BdrApplyConflict * bdr_make_apply_conflict(BdrConflictType conflict_type,
									BdrConflictResolution resolution,
									TransactionId remote_txid,
									BDRRelation *conflict_relation,
									struct TupleTableSlot *local_tuple,
									RepNodeId local_tuple_origin_id,
									struct TupleTableSlot *remote_tuple,
									struct ErrorData *apply_error);

extern void bdr_conflict_log_serverlog(BdrApplyConflict *conflict);
extern void bdr_conflict_log_table(BdrApplyConflict *conflict);

extern void tuple_to_stringinfo(StringInfo s, TupleDesc tupdesc, HeapTuple tuple);

#ifdef BUILDING_BDR
/* sequence support */
extern void bdr_sequencer_shmem_init(int nnodes, int sequencers);
extern void bdr_sequencer_init(int seq_slot, Size nnodes);
extern bool bdr_sequencer_vote(void);
extern void bdr_sequencer_tally(void);
extern void bdr_sequencer_start_elections(void);
extern void bdr_sequencer_fill_sequences(void);

extern void bdr_sequencer_wakeup(void);
extern void bdr_schedule_eoxact_sequencer_wakeup(void);

extern Datum bdr_sequence_alloc(PG_FUNCTION_ARGS);
extern Datum bdr_sequence_setval(PG_FUNCTION_ARGS);
extern Datum bdr_sequence_options(PG_FUNCTION_ARGS);
#endif

/* statistic functions */
extern void bdr_count_shmem_init(size_t nnodes);
extern void bdr_count_set_current_node(RepNodeId node_id);
extern void bdr_count_commit(void);
extern void bdr_count_rollback(void);
extern void bdr_count_insert(void);
extern void bdr_count_insert_conflict(void);
extern void bdr_count_update(void);
extern void bdr_count_update_conflict(void);
extern void bdr_count_delete(void);
extern void bdr_count_delete_conflict(void);
extern void bdr_count_disconnect(void);

/* compat check functions */
extern bool bdr_get_float4byval(void);
extern bool bdr_get_float8byval(void);
extern bool bdr_get_integer_timestamps(void);
extern bool bdr_get_bigendian(void);

/* initialize a new bdr member */
extern void bdr_init_replica(Name dbname);

/* shared memory management */
extern void bdr_maintain_schema(void);
extern BdrWorker* bdr_worker_shmem_alloc(BdrWorkerType worker_type,
										 uint32 *ctl_idx);
extern void bdr_worker_shmem_release(BdrWorker* worker, BackgroundWorkerHandle *handle);
extern bool bdr_is_bdr_activated_db(void);

/* forbid commands we do not support currently (or never will) */
extern void init_bdr_commandfilter(void);
extern void bdr_commandfilter_always_allow_ddl(bool always_allow);

extern void bdr_executor_init(void);
extern void bdr_executor_always_allow_writes(bool always_allow);
extern void bdr_queue_ddl_command(char *command_tag, char *command);
extern void bdr_execute_ddl_command(char *cmdstr, char *perpetrator, bool tx_just_started);

extern void bdr_locks_shmem_init(Size num_used_databases);
extern void bdr_locks_check_query(void);

/* background workers */
extern void bdr_apply_main(Datum main_arg);
extern void bdr_apply_work(PGconn* streamConn);

/* manipulation of bdr catalogs */
extern char bdr_nodes_get_local_status(uint64 sysid, TimeLineID tli, Oid dboid);
extern void bdr_nodes_set_local_status(char status);

extern Oid GetSysCacheOidError(int cacheId, Datum key1, Datum key2, Datum key3,
							   Datum key4);

#define GetSysCacheOidError2(cacheId, key1, key2) \
	GetSysCacheOidError(cacheId, key1, key2, 0, 0)



/* helpers shared by multiple worker types */
extern struct pg_conn* bdr_connect(char *conninfo_repl,
								   char *conninfo_db,
								   char* remote_ident,
								   size_t remote_ident_length,
								   NameData* slot_name,
								   uint64* remote_sysid_i,
								   TimeLineID *remote_tlid_i,
								   Oid *out_dboid_i);

extern struct pg_conn *
bdr_establish_connection_and_slot(BdrConnectionConfig *cfg,
								  const char *application_name_suffix,
								  Name out_slot_name,
								  uint64 *out_sysid,
								  TimeLineID *out_timeline,
								  Oid *out_dboid,
								  RepNodeId *out_replication_identifier,
								  char **out_snapshot);

/* use instead of heap_open()/heap_close() */
extern BDRRelation *bdr_heap_open(Oid reloid, LOCKMODE lockmode);
extern void bdr_heap_close(BDRRelation * rel, LOCKMODE lockmode);
extern void bdr_heap_compute_replication_settings(
	BDRRelation *rel,
	int			num_replication_sets,
	char	  **replication_sets);
extern void BDRRelcacheHashInvalidateCallback(Datum arg, Oid relid);

extern void bdr_parse_relation_options(const char *label, BDRRelation *rel);
extern void bdr_parse_database_options(const char *label);


/* conflict handlers API */
extern void bdr_conflict_handlers_init(void);

extern HeapTuple bdr_conflict_handlers_resolve(BDRRelation * rel,
											   const HeapTuple local,
											   const HeapTuple remote,
											   const char *command_tag,
											   BdrConflictType event_type,
											   uint64 timeframe, bool *skip);

/* replication set stuff */
void bdr_validate_replication_set_name(const char *name, bool allow_implicit);

/*
 * Global to identify the type of BDR worker the current process is. Primarily
 * useful for assertions and debugging.
 */
extern BdrWorkerType bdr_worker_type;

#endif   /* BDR_H */
