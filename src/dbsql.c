#include "common.h"
#include "dbsql.h"

int db_open()
{
	int rc, createdb = 0;
	char dbfilename[512];
	struct stat filestat;

	snprintf(dbfilename, 512, "%s/%s", cfg.dbdir, DATABASEFILE);

	/* create database if file doesn't exist */
	if (stat(dbfilename, &filestat) != 0) {
		if (errno == ENOENT) {
			createdb = 1;
		} else {
			if (debug)
				printf("Error: Handling database \"%s\" failed: %s\n", dbfilename, strerror(errno));
			return 0;
		}
	}

	rc = sqlite3_open(dbfilename, &db);

	if (rc) {
		if (debug)
			printf("Error: Can't open database \"%s\": %s\n", dbfilename, sqlite3_errmsg(db));
		return 0;
	} else {
		if (debug)
			printf("Database \"%s\" open\n", dbfilename);
	}

	if (createdb) {
		if (!db_create()) {
			if (debug)
				printf("Error: Creating database \"%s\" failed\n", dbfilename);
			return 0;
		} else {
			if (debug)
				printf("Database \"%s\" created\n", dbfilename);
		}
	}

	return 1;
}

int db_exec(char *sql)
{
	int rc;
	sqlite3_stmt *sqlstmt;

	rc = sqlite3_prepare_v2(db, sql, -1, &sqlstmt, NULL);
	if (rc) {
		if (debug)
			printf("Error: Insert prepare failed (%d): %s\n", rc, sqlite3_errmsg(db));
		return 0;
	}

	rc = sqlite3_step(sqlstmt);
	if (rc != SQLITE_DONE) {
		if (debug)
			printf("Error: Insert step failed (%d): %s\n", rc, sqlite3_errmsg(db));
		return 0;
	}

	rc = sqlite3_finalize(sqlstmt);
	if (rc) {
		if (debug)
			printf("Error: Finalize failed (%d): %s\n", rc, sqlite3_errmsg(db));
		return 0;
	}

	return 1;
}

int db_create()
{
	int rc, i;
	char *sql;
	char *datatables[] = {"fiveminute", "hour", "day", "month", "year"};

	/* TODO: check: COMMIT or END may be missing in error cases and return gets called before COMMIT */

	rc = sqlite3_exec(db, "BEGIN", 0, 0, 0);
	if (rc != 0) {
		return 0;
	}

	sql = "CREATE TABLE info(\n" \
		"  id       INTEGER PRIMARY KEY,\n" \
		"  name     TEXT UNIQUE NOT NULL,\n" \
		"  value    TEXT NOT NULL);";

	if (!db_exec(sql)) {
		return 0;
	}

	sql = "CREATE TABLE interface(\n" \
		"  id           INTEGER PRIMARY KEY,\n" \
		"  name         TEXT UNIQUE NOT NULL,\n" \
		"  alias        TEXT,\n" \
		"  active       INTEGER NOT NULL,\n" \
		"  created      DATE NOT NULL,\n" \
		"  updated      DATE NOT NULL,\n" \
		"  rxcounter    INTEGER NOT NULL,\n" \
		"  txcounter    INTEGER NOT NULL,\n" \
		"  rxtotal      INTEGER NOT NULL,\n" \
		"  txtotal      INTEGER NOT NULL);";

	if (!db_exec(sql)) {
		return 0;
	}

	for (i=0; i<5; i++) {
		if (debug)
			printf("%d: %s\n", i, datatables[i]);
		sql = malloc(sizeof(char)*512);

		snprintf(sql, 512, "CREATE TABLE %s(\n" \
			"  id           INTEGER PRIMARY KEY,\n" \
			"  interface    INTEGER REFERENCES interface ON DELETE CASCADE,\n" \
			"  date         DATE NOT NULL,\n" \
			"  rx           INTEGER NOT NULL,\n" \
			"  tx           INTEGER NOT NULL,\n" \
			"  CONSTRAINT u UNIQUE (interface, date));", datatables[i]);

		if (!db_exec(sql)) {
			free(sql);
			return 0;
		}
	}

	rc = sqlite3_exec(db, "COMMIT", 0, 0, 0);
	if (rc != 0) {
		return 0;
	}

	return 1;
}

int db_addinterface(char *iface)
{
	char sql[1024];

	snprintf(sql, 1024, "insert into interface (name, active, created, updated, rxcounter, txcounter, rxtotal, txtotal) values ('%s', 1, datetime('now'), datetime('now'), 0, 0, 0, 0);", iface);
	return db_exec(sql);
}

sqlite3_int64 db_getinterfaceid(char *iface)
{
	int rc;
	char sql[1024];
	sqlite3_int64 ifaceid = 0;
	sqlite3_stmt *sqlstmt;

	snprintf(sql, 1024, "select id from interface where name='%s'", iface);
	rc = sqlite3_prepare_v2(db, sql, -1, &sqlstmt, NULL);
	if (!rc) {
		if (sqlite3_step(sqlstmt) == SQLITE_ROW) {
			ifaceid = sqlite3_column_int64(sqlstmt, 0);
		}
		sqlite3_finalize(sqlstmt);
	}

	if (ifaceid == 0) {
		if (!db_addinterface(iface)) {
			return 0;
		}
		ifaceid = sqlite3_last_insert_rowid(db);
	}

	return ifaceid;
}

int db_addtraffic(char *iface, int rx, int tx)
{
	int i;
	char sql[1024];
	sqlite3_int64 ifaceid = 0;

	char *datatables[] = {"fiveminute", "hour", "day", "month", "year"};
	char *datadates[] = {"datetime('now', ('-' || (strftime('%M', 'now')) || ' minutes'), ('-' || (strftime('%S', 'now')) || ' seconds'), ('+' || (round(strftime('%M', 'now')/5,0)*5) || ' minutes'))", \
			"strftime('%Y-%m-%d %H:00:00', 'now')", \
			"date('now')", "strftime('%Y-%m-01', 'now')", \
			"strftime('%Y-01-01', 'now')"};

	ifaceid = db_getinterfaceid(iface);
	if (ifaceid == 0) {
		return 0;
	}

	sqlite3_exec(db, "BEGIN", 0, 0, 0);

	/* total */
	snprintf(sql, 1024, "update interface set rxtotal=rxtotal+%d, txtotal=txtotal+%d, updated=datetime('now') where id=%d;", rx, tx, (int)ifaceid);
	db_exec(sql);

	/* time specific */
	for (i=0; i<5; i++) {
		snprintf(sql, 1024, "insert or ignore into %s (interface, date, rx, tx) values (%d, %s, 0, 0);", datatables[i], (int)ifaceid, datadates[i]);
		db_exec(sql);
		snprintf(sql, 1024, "update %s set rx=rx+%d, tx=tx+%d where interface=%d and date=%s;", datatables[i], rx, tx, (int)ifaceid, datadates[i]);
		db_exec(sql);
	}

	sqlite3_exec(db, "COMMIT", 0, 0, 0);

	return 1;
}