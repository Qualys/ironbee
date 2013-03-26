/*****************************************************************************
 * Licensed to Qualys, Inc. (QUALYS) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * QUALYS licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ****************************************************************************/

#include "ironbee_config_auto.h"

#include <ironbee/kvstore_filesystem.h>

#include "kvstore_private.h"

#include <ironbee/clock.h>
#include <ironbee/kvstore.h>
#include <ironbee/util.h>
#include <ironbee/uuid.h>
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

/**
 * Define the width for printing an expiration time.
 * This is related to EXPIRE_STR_FMT.
 * Both are 12 to accommodate the typical 10 digits and 2 buffer digits
 * for extreme future-time use.
 */
static const size_t EXPIRE_FMT_WIDTH = 13;

/**
 * The length of a string representation of a UUID.
 */
static const size_t UUID_LEN_STR = 36;

/**
 * The sprintf format used for expiration times.
 */
#define EXPIRE_FMT "%012u"

/**
 * Define the width for printing a creation time
 * This is related to CREATE_STR_FMT.
 * for extreme future-time use.
 */
static const size_t CREATE_FMT_WIDTH = 17;

/**
 * The sprintf format used for expiration times.
 */
#define CREATE_FMT "%016"PRIx64


/**
 * Malloc and populate a filesystem path for a key/value pair.
 *
 * The path will include the key value, the expiration value, and the type
 * of the pattern:
 * @c &lt;base_path&gt;/&lt;key&gt;/&lt;expiration&gt;-&lt;type&gt;.
 *
 * @param[in] kvstore Key-Value store.
 * @param[in] key The key to write to.
 * @param[in] expiration The expiration in useconds. This is ignored
 *            if the argument type is NULL.
 * @param[in] type The type of the file. If null then expiration is
 *            ignored and a shortened path is generated
 *            representing only the directory.
 * @param[in] type_len The type length of the type_len. This value is ignored
 *            if expiration is < 0.
 * @param[in] prefix File name prefix (or NULL)
 * @param[in] suffix File name suffix (or NULL)
 * @param[out] path The malloc'ed path. The caller must free this.
 *             The path variable will be set to NULL if a failure occurs.
 *
 * @return
 *   - IB_OK on success
 *   - IB_EOTHER on system call failure. See @c errno.
 *   - IB_EALLOC if a memory allocation fails.
 */
static ib_status_t build_key_path(
    ib_kvstore_t *kvstore,
    const ib_kvstore_key_t *key,
    const ib_time_t expiration,
    const char *type,
    size_t type_len,
    const char *prefix,
    const char *suffix,
    char **path)
{
    assert(kvstore != NULL);
    assert(key != NULL);
    assert(path != NULL);

    /* System return code. */
    int sys_rc;

    /* IronBee return code */
    ib_status_t rc = IB_OK;

    /* A stat struct. sb is the name used in the man page example code. */
    struct stat sb;

    /* Constant size specified by ib_uuid_bin_to_ascii (minus the \0).*/
    size_t key_uuid_sz = UUID_LEN_STR+1;
    char *key_uuid_str = NULL;
    char *key_str = NULL; /* Null-terminated copy of the key. */
    char *path_base = NULL; /* Used to free path_tmp on failure. */
    size_t prefix_len = 0;
    size_t suffix_len = 0;
    size_t path_size;
    char *path_tmp; /* Used to manipulate the path we are building. */

    ib_kvstore_filesystem_server_t *server =
        (ib_kvstore_filesystem_server_t *)(kvstore->server);

    /* Clear output variable. */
    *path = NULL;

    if (prefix != NULL) {
        prefix_len = strlen(prefix);
    }

    if (suffix != NULL) {
        suffix_len = strlen(suffix);
    }

    key_str = kvstore->malloc(
        kvstore,
        key->length+1,
        kvstore->malloc_cbdata);
    if (key_str == NULL) {
        rc = IB_EALLOC;
        goto cleanup;
    }
    strncpy(key_str, key->key, key->length);
    key_str[key->length] = '\0';

    /* Allocate 37 byte block for the key to be written. */
    key_uuid_str = kvstore->malloc(
        kvstore,
        key_uuid_sz,
        kvstore->malloc_cbdata);
    if (key_uuid_str == NULL) {
        rc = IB_EALLOC;
        goto cleanup;
    }

    rc = ib_uuid_create_v5_str(&key_uuid_str, &key_uuid_sz, key_str);
    if (rc != IB_OK) {
        goto cleanup;
    }

    path_size =
        server->directory_length /* length of path */
        + 1                      /* path separator */
        + UUID_LEN_STR           /* key length */
        + 1                      /* path separator */
        + prefix_len             /* Prefix length */
        + EXPIRE_FMT_WIDTH       /* width to format the expiration time. */
        + 1                      /* dash. */
        + CREATE_FMT_WIDTH       /* width to format a creation time. */
        + 1                      /* dot. */
        + type_len               /* type. */
        + suffix_len             /* Suffix length. */
        + 1                      /* '\0' */;

    path_tmp = kvstore->malloc(
        kvstore,
        path_size+1,
        kvstore->malloc_cbdata);
    if (path_tmp == NULL) {
        rc = IB_EALLOC;
        goto cleanup;
    }
    path_base = path_tmp;

    /* Push allocated path back to user. We now populate it. */
    *path = path_tmp;

    /* Append the path to the directory. */
    path_tmp = (strncpy(path_tmp, server->directory, server->directory_length)
                + server->directory_length);

    /* Append the key. */
    path_tmp = strncpy(path_tmp, "/", 1) + 1;
    path_tmp = strncpy(path_tmp, key_uuid_str, UUID_LEN_STR) + UUID_LEN_STR;

    /* Momentarily tag the end of the path for the stat check. */
    *path_tmp = '\0';
    errno = 0;

    /* Check for a key directory. Make one if able.*/
    sys_rc = stat(*path, &sb);
    if (errno == ENOENT) {
        sys_rc = mkdir(*path, 0700);

        if (sys_rc) {
            rc = IB_EOTHER;
            goto cleanup;
        }
    }
    else if (sys_rc) {
        rc = IB_EOTHER;
        goto cleanup;
    }
    else if (!S_ISDIR(sb.st_mode)) {
        rc = IB_EOTHER;
        goto cleanup;
    }

    if (type != NULL) {
        ib_timeval_t tv;
        uint32_t expire_time_seconds;
        ib_time_t create_time;

        ib_clock_gettimeofday(&tv);
        create_time = IB_CLOCK_TIMEVAL_TIME(tv);
        if (expiration != 0) {
            expire_time_seconds = IB_CLOCK_SECS(create_time + expiration);
        }
        else {
            expire_time_seconds = 0;
        }

        path_tmp = strncpy(path_tmp, "/", 1) + 1;
        if (prefix != NULL) {
            path_tmp = strcpy(path_tmp, prefix);
        }
        path_tmp += snprintf(
            path_tmp,
            EXPIRE_FMT_WIDTH + 1 + CREATE_FMT_WIDTH,
            EXPIRE_FMT "-" CREATE_FMT,
            expire_time_seconds, create_time);
        path_tmp = strncpy(path_tmp, ".", 1) + 1;
        path_tmp = strncpy(path_tmp, type, type_len) + type_len;
        if (suffix != NULL) {
            path_tmp = strcpy(path_tmp, suffix) + suffix_len;
        }
    }
    *path_tmp = '\0';

cleanup:
    if (rc == IB_OK) {
        *path = path_base;
    }
    else {
        if (path_base) {
            kvstore->free(kvstore, path_base, kvstore->free_cbdata);
        }
    }

    if (key_uuid_str) {
        kvstore->free(kvstore, key_uuid_str, kvstore->free_cbdata);
    }

    if (key_str) {
        kvstore->free(kvstore, key_str, kvstore->free_cbdata);
    }

    return rc;
}

static ib_status_t kvconnect(
    ib_kvstore_t *kvstore,
    ib_kvstore_cbdata_t *cbdata)
{
    assert(kvstore);

    /* Nop. */

    return IB_OK;
}

static ib_status_t kvdisconnect(
    ib_kvstore_t *kvstore,
    ib_kvstore_cbdata_t *cbdata)
{
    assert(kvstore);

    /* Nop. */

    return IB_OK;
}

/**
 * Read a whole file into data.
 *
 * @param[in] kvstore The key-value store.
 * @param[in] path The path to the file.
 * @param[out] data A kvstore->malloc'ed array containing the file data.
 * @param[out] len The length of the data buffer.
 *
 * @returns
 *   - IB_OK on ok.
 *   - IB_EALLOC on memory allocation error.
 *   - IB_EOTHER on system call failure.
 */
static ib_status_t read_whole_file(
    ib_kvstore_t *kvstore,
    const char *path,
    void **data,
    size_t *len)
{
    assert(kvstore);
    assert(path);

    struct stat sb;
    int sys_rc;
    int fd = -1;
    char *dataptr;
    char *dataend;

    sys_rc = stat(path, &sb);
    if (sys_rc) {
        return IB_EOTHER;
    }

    dataptr = kvstore->malloc(
        kvstore,
        sb.st_size,
        kvstore->malloc_cbdata);
    if (!dataptr) {
        return IB_EALLOC;
    }
    *len = sb.st_size;
    *data = dataptr;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        goto eother_failure;
    }

    for (dataend = dataptr + sb.st_size; dataptr < dataend; )
    {
        ssize_t fr_tmp = read(fd, dataptr, dataend - dataptr);

        if (fr_tmp < 0) {
            goto eother_failure;
        }

        dataptr += fr_tmp;
    }

    close(fd);

    return IB_OK;

eother_failure:
    if (fd >= 0) {
        close(fd);
    }
    kvstore->free(kvstore, *data, kvstore->free_cbdata);
    *data = NULL;
    *len = 0;
    return IB_EOTHER;
}

/**
 * Extract time information from a file name
 *
 * The file name is expected to have one of the two formats from kvset().
 * That is: "<expiration>-<creation>.<type>.XXXXXX", or
 * ".<expiration>-<creation>.<type>.XXXXXX".  The leading '.', if present,
 * indicates that the file is a temporary file.  The expiration time is then
 * extracted, followed by the creation time, which is stored as a 16-digit
 * hexadecimal string.
 *
 * @param[in] kvstore Key-value store (unused)
 * @param[in] fname File name to extract from
 * @param[out] is_temp_file true if @a path refers to a temporary file
 * @param[out] expiration Expiration time from @a path
 * @param[out] creation Creation time from @a path
 *
 * @returns
 *   - IB_OK on success.
 *   - IB_EALLOC on memory allocation failures.
 *   - IB_ENOENT if a / and . characters are not found to denote
 *               the location of the expiration decimals.
 */
static ib_status_t extract_time_info(
    ib_kvstore_t *kvstore,
    const char *fname,
    bool *is_temp_file,
    ib_time_t *expiration,
    ib_time_t *creation)
{
    assert(kvstore != NULL);
    assert(fname != NULL);
    assert(is_temp_file != NULL);
    assert(expiration != NULL);
    assert(creation != NULL);

    char *pos = (char *)fname;          /* stroll() takes a char * as arg 2 */

    if (*pos == '.') {
        ++pos;                          /* Ignore leading "." on file name */
        *is_temp_file = true;
    }
    else {
        *is_temp_file = false;
    }
    if (!isdigit(*pos) ) {
        return IB_EINVAL;
    }
    *expiration = strtoll(pos, &pos, 10) * 1000000;

    /* Skip the '-' */
    if (*pos != '-') {
        return IB_EINVAL;
    }
    ++pos;
    *creation = strtoll(pos, &pos, 16);

    return IB_OK;
}

/**
 * Extract the type information from a file name.  See extract_time_info() for
 * details of the filename format.
 *
 * @param[in] kvstore Key-value store
 * @param[in] fname File name to extract from
 * @param[out] type Copy of the type name
 * @param[out] type_length Length of @a type.
 *
 * @returns
 *   - IB_OK on success.
 *   - IB_EALLOC on memory allocation failures.
 *   - IB_ENOENT if a / and . characters are not found to denote
 *               the location of the expiration decimals.
 */
static ib_status_t extract_type(
    ib_kvstore_t *kvstore,
    const char *fname,
    char **type,
    size_t *type_length)
{
    assert(kvstore != NULL);
    assert(fname != NULL);
    assert(type != NULL);
    assert(type_length != NULL);

    const char *pos;
    size_t len;

    pos = strchr(fname, '.');
    if (pos == NULL) {
        return IB_EINVAL;
    }
    len = strlen(pos) - 8;  /* Ignore '.' and trailing .XXXXXX from mkstemp() */
    *type = kvstore->malloc(
        kvstore,
        len+1,
        kvstore->malloc_cbdata);
    if (!*type) {
        return IB_EALLOC;
    }
    strncpy(*type, pos, len);
    *type_length = len;

    return IB_OK;
}

/**
 * Load a value from a key-value store
 *
 * This function looks at the file name passed in, and calls
 * extract_time_info() to get the expiration time, creation time and whether
 * or not the file is a temporary file.  If the file is found to be expired,
 * it's deleted whether it's a temporary file or not; non-expired temporary
 * files are ignored.
 *
 * For other files, the type of the file is extracted from the file name, and
 * then the contents of the file are read into the the value structure.
 *
 * @param[in] kvstore Key-value store.
 * @param[in] dpath Directory path
 * @param[in] fname The file name.
 * @param[out] pvalue Pointer to the value to be built.
 *
 * @returns
 *   - IB_OK on success.
 *   - IB_EALLOC on memory failure from @c kvstore->malloc
 *   - IB_EOTHER on system call failure from file operations.
 *   - IB_DECLINED returned when @a fname is found to be either
 *     expired or a temporary file
 */
static ib_status_t load_kv_value(
    ib_kvstore_t *kvstore,
    const char *dpath,
    const char *fname,
    ib_kvstore_value_t **pvalue)
{
    assert(kvstore != NULL);
    assert(dpath != NULL);
    assert(fname != NULL);
    assert(pvalue != NULL);

    ib_status_t rc;
    ib_timeval_t ib_timeval;
    ib_time_t expiration;
    ib_time_t creation;
    ib_time_t now;
    bool is_temp;
    ib_kvstore_value_t *value = NULL;
    char *file_path = NULL;
    size_t len;

    *pvalue = NULL;

    /* Extract time info.  Log invalid file names and ignore them. */
    rc = extract_time_info(kvstore, fname, &is_temp, &expiration, &creation);
    if (rc == IB_EINVAL) {
        ib_util_log_error("kvstore: Ignoring file with invalid name \"%s\"",
                          fname);
        return IB_OK;
    }
    else if (rc != IB_OK) {
        return rc;
    }

    /* Build full path. */
    len = strlen(dpath) + strlen(fname) + 2;
    file_path = kvstore->malloc(kvstore, len, kvstore->malloc_cbdata);
    if (file_path == NULL) {
        return IB_EALLOC;
    }
    strcpy(file_path, dpath);
    strcat(file_path, "/");
    strcat(file_path, fname);

    /* Get the current time */
    ib_clock_gettimeofday(&ib_timeval);
    now = IB_CLOCK_TIMEVAL_TIME(ib_timeval);

    /* Remove expired file and signal there is no entry for that file. */
    if (now > expiration) {
        rc = IB_DECLINED;

        /* Remove the expired file. */
        unlink(file_path);

        /* Try to remove the key directory, though it may not be empty.
         * Failure is OK. */
        rmdir(dpath);
        goto cleanup;
    }

    /* Decline */
    if (is_temp) {
        rc = IB_DECLINED;
        goto cleanup;
    }

    /* Use kvstore->malloc because of framework contractual requirements. */
    value = kvstore->malloc(kvstore, sizeof(*value), kvstore->malloc_cbdata);
    if (value == NULL) {
        rc = IB_EALLOC;
        goto cleanup;
    }

    /* Populate expiration & creation times. */
    value->expiration = expiration;
    value->creation   = creation;

    /* Populate type and type_length. */
    rc = extract_type(kvstore, fname, &(value->type), &(value->type_length));
    if (rc != IB_OK) {
        rc = IB_EOTHER;
        goto cleanup;
    }

    /* Populate value and value_length. */
    rc = read_whole_file(kvstore, file_path,
                         &(value->value), &(value->value_length));
    if (rc != IB_OK) {
        rc = IB_EOTHER;
        goto cleanup;
    }

cleanup:
    if (file_path != NULL) {
        kvstore->free(kvstore, file_path, kvstore->free_cbdata);
    }
    if ( (rc != IB_OK) && (value != NULL) ) {
        if (value->type != NULL) {
            kvstore->free(kvstore, value->type, kvstore->free_cbdata);
        }
        if (value->value != NULL) {
            kvstore->free(kvstore, value->value, kvstore->free_cbdata);
        }
        kvstore->free(kvstore, value, kvstore->free_cbdata);
    }
    else {
        *pvalue = value;
    }
    return rc;
}

typedef ib_status_t(*each_dir_t)(const char *path, const char *dirent, void *);

/**
 * Count the entries that do not begin with a dot.
 *
 * @param[in] path The directory path.
 * @param[in] dirent The directory entry in path.
 * @param[in,out] data An int* representing the current count of directories.
 *
 * @returns IB_OK.
 */
static ib_status_t count_dirent(
    const char *path,
    const char *dirent,
    void *data)
{
    assert(path != NULL);
    assert(dirent != NULL);
    assert(data != NULL);

    size_t *i = (size_t *)data;

    if ( (*dirent != '.') || (strlen(dirent) > 2) ) {
        ++(*i);
    }

    return IB_OK;
}

/**
 * Iterate through a directory in a standard way.
 *
 * @returns
 *   - IB_OK on success.
 *   - Other errors based on each_dir_t.
 */
static ib_status_t each_dir(const char *path, each_dir_t f, void* data)
{
    assert(path);
    assert(f);

    int tmp_errno; /* Holds errno until other system calls finish. */
    int sys_rc;
    ib_status_t rc;

    size_t dirent_sz;      /* Size of a dirent structure. */
    DIR *dir = NULL;
    struct dirent *entry = NULL;
    struct dirent *result = NULL;

    dir = opendir(path);

    if (!dir) {
        rc = IB_EOTHER;
        goto rc_failure;
    }

    /* Man page-specified to portably allocate the dirent buffer for entry. */
    dirent_sz =
        (
            offsetof(struct dirent, d_name) +
            pathconf(path, _PC_NAME_MAX) + 1 + sizeof(long)
        ) & -sizeof(long);

    entry = malloc(dirent_sz);

    if (!entry) {
        rc = IB_EOTHER;
        goto rc_failure;
    }

    while (1) {
        sys_rc = readdir_r(dir, entry, &result);
        if (sys_rc) {
            rc = IB_EOTHER;
            goto rc_failure;
        }
        if (result == NULL) {
            break;
        }
        rc = f(path, result->d_name, data);
        if ( (rc != IB_OK) && (rc != IB_DECLINED) ) {
            goto rc_failure;
        }
    }

    /* Clean exit. */
    closedir(dir);
    free(entry);
    return IB_OK;

rc_failure:
    tmp_errno = errno;
    if (dir) {
        closedir(dir);
    }
    if (entry) {
        free(entry);
    }
    errno = tmp_errno;
    return rc;
}

/**
 * Callback user data structure for build_value callback function.
 */
struct build_value_t {
    ib_kvstore_t *kvstore;        /**< Key value store. */
    ib_kvstore_value_t **values;  /**< Values array to be build. */
    size_t values_idx;         /**< Next value to be populated. */
    size_t values_len;         /**< Prevent new file causing array overflow. */
    size_t path_len;           /**< Cached path length value. */
};
typedef struct build_value_t build_value_t;

/**
 * Build an array of values.
 *
 * @param[in] path The directory path.
 * @param[in] file The file to load.
 * @param[out] data The data structure to be populated.
 * @returns
 *   - IB_OK
 *   - IB_EALLOC On a memory error.
 */
static ib_status_t build_value(const char *path, const char *file, void *data)
{
    assert(path);
    assert(file);
    assert(data);

    ib_status_t rc;
    build_value_t *bv = (build_value_t *)(data);
    ib_kvstore_value_t *value;

    /* Return if there is no space left in our array.
     * Partial results are not an error as an asynchronous write may
     * create a new file. */
    if (bv->values_idx >= bv->values_len) {
        return IB_OK;
    }

    if ( (*file == '.') && (strlen(file) <= 2) ) {
        return IB_OK;
    }
    rc = load_kv_value(bv->kvstore, path, file, &value);

    /* DECLINE means expired or temporary file; ignore it.
     * If OK, add the new value to the list */
    if (rc == IB_DECLINED) {
        return IB_OK;
    }
    else if (rc == IB_OK) {
        *(bv->values + bv->values_idx) = value;
        bv->values_idx++;
    }

    return IB_OK;
}

/**
 * Get implementations.
 *
 * @param[in] kvstore The key-value store.
 * @param[in] key The key to fetch.
 * @param[out] values A pointer to an array of pointers.
 * @param[out] values_length The length of *values.
 * @param[in,out] cbdata Callback data. Unused.
 */
static ib_status_t kvget(
    ib_kvstore_t *kvstore,
    const ib_kvstore_key_t *key,
    ib_kvstore_value_t ***values,
    size_t *values_length,
    ib_kvstore_cbdata_t *cbdata)
{
    assert(kvstore);
    assert(key);

    ib_status_t rc;
    build_value_t build_val;
    char *path = NULL;
    size_t dirent_count = 0;

    /* Build a path with no expiration value on it. */
    rc = build_key_path(kvstore, key, 0, NULL, 0, NULL, NULL, &path);
    if (rc != IB_OK) {
        goto failure1;
    }

    /* Count entries. */
    rc = each_dir(path, &count_dirent, &dirent_count);
    if (rc != IB_OK) {
        goto failure1;
    }
    if (dirent_count == 0){
        rc = IB_ENOENT;
        goto failure1;
    }

    /* Initialize build_values user data. */
    build_val.kvstore = kvstore;
    build_val.path_len = strlen(path);
    build_val.values_idx = 0;
    build_val.values_len = dirent_count;
    build_val.values = (ib_kvstore_value_t**)kvstore->malloc(
        kvstore,
        sizeof(*build_val.values) * dirent_count,
        kvstore->malloc_cbdata);
    if (build_val.values == NULL) {
        rc = IB_EALLOC;
        goto failure1;
    }

    /* Build value array. */
    rc = each_dir(path, &build_value, &build_val);
    if (rc != IB_OK) {
        goto failure2;
    }

    /* Commit back results.
     * NOTE: values_idx does not need a +1 because it points at the
     *       next empty slot in the values array. */
    *values = build_val.values;
    *values_length = build_val.values_idx;

    /* Clean exit. */
    kvstore->free(kvstore, path, kvstore->free_cbdata);
    return IB_OK;

    /**
     * Reverse initialization error labels.
     */
failure2:
    kvstore->free(kvstore, build_val.values, kvstore->free_cbdata);
failure1:
    if (path) {
        kvstore->free(kvstore, path, kvstore->free_cbdata);
    }
    return rc;
}

/**
 * Set callback.
 *
 * This function creates 2 files with mkstemp().  The first file (path_real),
 * has a file name format "<expiration>-<creation>.<type>.XXXXXX".  The second
 * file (path_tmp), has an identical layout, but with a leading ".", so it's
 * ".<expiration>-<creation>.<type>.XXXXXX".  The "real" file is a place
 * holder, to prevent other processes / threads from writing to the same file.
 * The "temporary" file (with the leading '.'), is created, written to,
 * closed, and then renamed on top of the real file.  Thus, we get a 2-phase
 * commit with guaranteed file name uniqueness.
 *
 * The reader code (load_kv_value()), will ignore file names that start with a
 * '.', unless the file is expired.
 *
 * @param[in] kvstore Key-value store.
 * @param[in] merge_policy This implementation does not merge on writes.
 *            Merging is done by the framework on reads.
 * @param[in] key The key to fetch all values for.
 * @param[in] value The value to write. The framework contract says that this
 *            is also an out-parameters, but in this implementation the
 *            merge_policy is not used so value is never merged
 *            and never written to.
 * @param[in,out] cbdata Callback data for the user.
 */
static ib_status_t kvset(
    ib_kvstore_t *kvstore,
    ib_kvstore_merge_policy_fn_t merge_policy,
    const ib_kvstore_key_t *key,
    ib_kvstore_value_t *value,
    ib_kvstore_cbdata_t *cbdata)
{
    assert(kvstore);
    assert(key);
    assert(value);

    ib_status_t rc;
    char *path_real = NULL;
    char *path_tmp = NULL;
    int fd = -1;
    int sys_rc;
    ssize_t written;

    /* Build a path with expiration value in it. */
    rc = build_key_path(
        kvstore,
        key,
        value->expiration,
        value->type,
        value->type_length,
        NULL,
        ".XXXXXX",      /* ".XXXXXX" suffix for mkstemp() */
        &path_real);
    if (rc != IB_OK) {
        goto cleanup;
    }
    fd = mkstemp(path_real);
    if (fd < 0) {
        rc = IB_EOTHER;
        goto cleanup;
    }

    /* Close this file immediately; it's just a place holder,
     * and we're not going to write to it */
    close(fd);
    fd = -1;

    /* Build a path with expiration value in it. */
    rc = build_key_path(
        kvstore,
        key,
        value->expiration,
        value->type,
        value->type_length,
        ".",            /* Start the file name with a "." */
        ".XXXXXX",      /* ".XXXXXX" suffix for mkstemp() */
        &path_tmp);
    if (rc != IB_OK) {
        goto cleanup;
    }
    fd = mkstemp(path_tmp);
    if (fd < 0) {
        rc = IB_EOTHER;
        goto cleanup;
    }

    /* Write to the tmp file. */
    written = write(fd, value->value, value->value_length);
    if (written < (ssize_t)value->value_length ){
        rc = IB_EOTHER;
        goto cleanup;
    }
    close(fd);

    /* Now, rename the temp file to the real file */
    sys_rc = rename(path_tmp, path_real);
    if (sys_rc < 0) {
        rc = IB_EOTHER;
        goto cleanup;
    }

    rc = IB_OK;

cleanup:
    if (fd >= 0) {
        close(fd);
    }
    if (path_real != NULL) {
        kvstore->free(kvstore, path_real, kvstore->free_cbdata);
    }
    if (path_tmp != NULL) {
        kvstore->free(kvstore, path_tmp, kvstore->free_cbdata);
    }
    return rc;
}

/**
 * Remove all entries from key directory.
 *
 * @param[in] path The path to the key directory. @c rmdir is called on this.
 * @param[in] file The file inside of the directory pointed to by path
 *            which will be @c unlink'ed.
 * @param[in,out] data This is a size_t pointer containing
 *                the length of the path argument.
 * @returns
 *   - IB_OK
 *   - IB_EALLOC if a memory allocation fails concatenating path and file.
 */
static ib_status_t remove_file(
    const char *path,
    const char *file,
    void *data)
{
    assert(path);
    assert(file);
    assert(data);

    char *full_path;
    size_t path_len = *(size_t *)(data);

    if (*file != '.') {
        /* Build full path. */
        full_path = malloc(path_len + strlen(file) + 2);

        if (!full_path) {
            return IB_EALLOC;
        }

        sprintf(full_path, "%s/%s", path, file);

        unlink(full_path);

        free(full_path);
    }

    return IB_OK;
}

/**
 * Remove a key from the store.
 *
 * @param[in] kvstore
 * @param[in] key
 * @param[in,out] cbdata Callback data.
 * @returns
 *   - IB_OK on success.
 *   - IB_EOTHER on system call failure in build_key_path. See @c errno.
 *   - IB_EALLOC if a memory allocation fails.
 */
static ib_status_t kvremove(
    ib_kvstore_t *kvstore,
    const ib_kvstore_key_t *key,
    ib_kvstore_cbdata_t *cbdata)
{
    assert(kvstore);
    assert(key);

    ib_status_t rc;
    char *path = NULL;
    size_t path_len;

    /* Build a path with no expiration value on it. */
    rc = build_key_path(kvstore, key, 0, NULL, 0, NULL, NULL, &path);
    if (rc != IB_OK) {
        return rc;
    }

    path_len = strlen(path);

    /* Remove all files in the key directory. */
    each_dir(path, remove_file, &path_len);

    /* Attempt to remove the key path. This may fail, and that is OK.
     * Another process may write a new key while we were deleting old ones. */
    rmdir(path);

    free(path);

    return IB_OK;
}

/**
 * Destroy any allocated elements of the kvstore structure.
 * @param[out] kvstore to be destroyed. The contents on disk is untouched
 *             and another init of kvstore pointing at that directory
 *             will operate correctly.
 * @param[in] cbdata Unused.
 */
static void kvdestroy (ib_kvstore_t* kvstore, ib_kvstore_cbdata_t *cbdata)
{
    assert(kvstore);

    ib_kvstore_filesystem_server_t *server =
        (ib_kvstore_filesystem_server_t *)(kvstore->server);
    free((void *)server->directory);
    free(server);
    kvstore->server = NULL;

    return;
}

ib_status_t ib_kvstore_filesystem_init(
    ib_kvstore_t* kvstore,
    const char* directory)
{
    assert(kvstore);
    assert(directory);

    /* There is no callback data used for this implementation. */
    ib_kvstore_init(kvstore);

    ib_kvstore_filesystem_server_t *server = malloc(sizeof(*server));

    if ( server == NULL ) {
        return IB_EALLOC;
    }

    server->directory = strdup(directory);
    server->directory_length = strlen(directory);

    if ( server->directory == NULL ) {
        free(server);
        return IB_EALLOC;
    }

    kvstore->server = (ib_kvstore_server_t *) server;
    kvstore->get = kvget;
    kvstore->set = kvset;
    kvstore->remove = kvremove;
    kvstore->connect = kvconnect;
    kvstore->disconnect = kvdisconnect;
    kvstore->destroy = kvdestroy;

    kvstore->malloc_cbdata = NULL;
    kvstore->free_cbdata = NULL;
    kvstore->connect_cbdata = NULL;
    kvstore->disconnect_cbdata = NULL;
    kvstore->get_cbdata = NULL;
    kvstore->set_cbdata = NULL;
    kvstore->remove_cbdata = NULL;
    kvstore->merge_policy_cbdata = NULL;
    kvstore->destroy_cbdata = NULL;

    return IB_OK;
}
