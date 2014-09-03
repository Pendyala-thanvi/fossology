/*
Author: Daniele Fognini, Andreas Wuerl
Copyright (C) 2013-2014, Siemens AG

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License version 2 as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#define _GNU_SOURCE
#include <libfossology.h>
#include <string.h>
#include <stddef.h>

#include "bulk.h"
#include "file_operations.h"
#include "database.h"
#include "license.h"
#include "match.h"
#include "monk.h"

int parseBulkArguments(int argc, char** argv, MonkState* state) {
  if (argc < 1)
    return 0;
  /* TODO use normal cli arguments instead of this indian string
   *
   * at the moment it is separated by '\31' == \x19
   *  B or N, uploadId, uploadTreeId, licenseName, userId, refText, groupName, fullLicenseName
   *
   *  for example run
   *  monk $'B\x191\x19a\x19\x19shortname\x192\x19copyrights\x19group\x19fullname'
   */

  // TODO remove magics from here
  char* delimiters = "\31";
  char* remainder = NULL;
  char* argumentsCopy = g_strdup(argv[1]);

  char* tempargs[8];
  unsigned int index = 0;

  char* tokenString = strtok_r(argumentsCopy, delimiters, &remainder);
  while (tokenString != NULL && index < sizeof(tempargs)) {
    tempargs[index++] = tokenString;
    tokenString = strtok_r(NULL, delimiters, &remainder);
  }

  int result = 0;
  // TODO remove magics from here
  if ((tokenString == NULL) &&
    ((strcmp(tempargs[0], "B") == 0) || (strcmp(tempargs[0], "N") == 0)) &&
    (index >= 6))
  {
    BulkArguments* bulkArguments = malloc(sizeof(BulkArguments));

    bulkArguments->removing = (tempargs[0][0] == 'N');
    bulkArguments->userId = atoi(tempargs[1]);
    bulkArguments->groupId = atoi(tempargs[2]);
    bulkArguments->uploadTreeId = atol(tempargs[3]);
    bulkArguments->licenseId = atol(tempargs[4]);
    bulkArguments->refText = g_strdup(tempargs[5]);

    state->bulkArguments = bulkArguments;

    result = 1;
  }

  g_free(argumentsCopy);
  return result;
}

void bulkArguments_contents_free(BulkArguments* bulkArguments) {
  g_free(bulkArguments->refText);

  free(bulkArguments);
}

int bulk_identification(MonkState* state) {
  BulkArguments* bulkArguments = state->bulkArguments;

  License license = (License){
    .refId = bulkArguments->licenseId,
    .shortname = "unused" // we could query it, but we do not need it
  };
  license.tokens = tokenize(bulkArguments->refText, DELIMITERS);

  GArray* licenses = g_array_new(TRUE, FALSE, sizeof (License));
  g_array_append_val(licenses, license);

  PGresult* filesResult = queryFileIdsForUpload(state->dbManager,
                                                bulkArguments->uploadId);
  int haveError = 1;
  if (filesResult != NULL) {
    int resultsCount = PQntuples(filesResult);
    haveError = 0;
#ifdef MONK_MULTI_THREAD
    #pragma omp parallel
#endif
    {
      MonkState threadLocalStateStore = *state;
      MonkState* threadLocalState = &threadLocalStateStore;

      threadLocalState->dbManager = fo_dbManager_fork(state->dbManager);
      if (threadLocalState->dbManager) {
#ifdef MONK_MULTI_THREAD
        #pragma omp for
#endif
        for (int i = 0; i<resultsCount; i++) {
          long fileId = atol(PQgetvalue(filesResult, i, 0));

          // this will call onFullMatch_Bulk if it finds matches
          matchPFileWithLicenses(threadLocalState, fileId, licenses);
          fo_scheduler_heart(1);
        }
        fo_dbManager_finish(threadLocalState->dbManager);
      } else {
        haveError = 1;
      }
    }
    PQclear(filesResult);
  }

  freeLicenseArray(licenses);

  return !haveError;
}

int handleBulkMode(MonkState* state) {
  BulkArguments* bulkArguments = state->bulkArguments;

  bulkArguments->uploadId = queryUploadIdFromTreeId(state->dbManager,
                                                    bulkArguments->uploadTreeId);

  int arsId = fo_WriteARS(fo_dbManager_getWrappedConnection(state->dbManager),
                          0, bulkArguments->uploadId, state->agentId, AGENT_ARS, NULL, 0);

  int result = bulk_identification(state);

  fo_WriteARS(fo_dbManager_getWrappedConnection(state->dbManager),
              arsId, bulkArguments->uploadId, state->agentId, AGENT_ARS, NULL, 1);

  return result;
}

void onFullMatch_Bulk(MonkState* state, File* file, License* license, DiffMatchInfo* matchInfo) {
  int removed = state->bulkArguments->removing ? 1 : 0;

if (0) {
  printf("found bulk match: fileId=%ld, licId=%ld, ", file->id, license->refId);
  printf("start: %zu, length: %zu, ", matchInfo->text.start, matchInfo->text.length);
  printf("removed: %d\n", removed);
}
  /* TODO write correct query after changing the db format */
  //TODO we also want to save highlights

  /* we add a clearing decision for each uploadtree_fk corresponding to this pfile_fk
   * For each bulk scan scan we only have a n the other hand we have only one license per clearing decision
   */
  PGresult* insertResult = fo_dbManager_ExecPrepared(
    fo_dbManager_PrepareStamement(
      state->dbManager,
      "saveBulkResult",
      "WITH clearingIds AS ("
      " INSERT INTO clearing_decision(uploadtree_fk, pfile_fk, user_fk, type_fk, scope_fk)"
      "  SELECT uploadtree_pk, $1, $2, type_pk, scope_pk"
      "  FROM uploadtree, clearing_decision_types, clearing_decision_scopes"
      "  WHERE upload_fk = $3 AND pfile_fk = $1 "
      "  AND clearing_decision_types.meaning = '" BULK_DECISION_TYPE "'"
      "  AND clearing_decision_scopes.meaning = '" BULK_DECISION_SCOPE "'"
      " RETURNING clearing_pk "
      ")"
      "INSERT INTO clearing_licenses(clearing_fk, rf_fk, removed) "
      "SELECT clearing_pk,$4,$5 FROM clearingIds",
      long, long, int, long, int
    ),
    file->id,
    state->bulkArguments->userId,
    state->bulkArguments->uploadId,
    license->refId,
    removed
  );

  /* ignore errors */
  if (insertResult)
    PQclear(insertResult);
}