#include <config.h>

#include <time.h>

#include "internal.h"
#include "virhash.h"
#include "virhashdata.h"
#include "testutils.h"
#include "viralloc.h"
#include "virlog.h"
#include "virstring.h"

#define VIR_FROM_THIS VIR_FROM_NONE

VIR_LOG_INIT("tests.hashtest");

static virHashTablePtr
testHashInit(void)
{
    virHashTablePtr hash;
    ssize_t i;

    if (!(hash = virHashNew(NULL)))
        return NULL;

    /* entries are added in reverse order so that they will be linked in
     * collision list in the same order as in the uuids array
     */
    for (i = G_N_ELEMENTS(uuids) - 1; i >= 0; i--) {
        if (virHashAddEntry(hash, uuids[i], (void *) uuids[i]) < 0) {
            virHashFree(hash);
            return NULL;
        }
    }

    for (i = 0; i < G_N_ELEMENTS(uuids); i++) {
        if (!virHashLookup(hash, uuids[i])) {
            VIR_TEST_VERBOSE("\nentry \"%s\" could not be found", uuids[i]);
            virHashFree(hash);
            return NULL;
        }
    }

    return hash;
}

static int
testHashCheckForEachCount(void *payload G_GNUC_UNUSED,
                          const char *name G_GNUC_UNUSED,
                          void *data G_GNUC_UNUSED)
{
    size_t *count = data;
    *count += 1;
    return 0;
}

static int
testHashCheckCount(virHashTablePtr hash, size_t count)
{
    size_t iter_count = 0;

    if (virHashSize(hash) != count) {
        VIR_TEST_VERBOSE("\nhash contains %zd instead of %zu elements",
                         virHashSize(hash), count);
        return -1;
    }

    virHashForEach(hash, testHashCheckForEachCount, &iter_count);
    if (count != iter_count) {
        VIR_TEST_VERBOSE("\nhash claims to have %zu elements but iteration"
                         "finds %zu", count, iter_count);
        return -1;
    }

    return 0;
}


struct testInfo {
    void *data;
    size_t count;
};


static int
testHashGrow(const void *data G_GNUC_UNUSED)
{
    virHashTablePtr hash;
    int ret = -1;

    if (!(hash = testHashInit()))
        return -1;

    if (testHashCheckCount(hash, G_N_ELEMENTS(uuids)) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virHashFree(hash);
    return ret;
}


static int
testHashUpdate(const void *data G_GNUC_UNUSED)
{
    int count = G_N_ELEMENTS(uuids) + G_N_ELEMENTS(uuids_new);
    virHashTablePtr hash;
    size_t i;
    int ret = -1;

    if (!(hash = testHashInit()))
        return -1;

    for (i = 0; i < G_N_ELEMENTS(uuids_subset); i++) {
        if (virHashUpdateEntry(hash, uuids_subset[i], (void *) 1) < 0) {
            VIR_TEST_VERBOSE("\nentry \"%s\" could not be updated",
                    uuids_subset[i]);
            goto cleanup;
        }
    }

    for (i = 0; i < G_N_ELEMENTS(uuids_new); i++) {
        if (virHashUpdateEntry(hash, uuids_new[i], (void *) 1) < 0) {
            VIR_TEST_VERBOSE("\nnew entry \"%s\" could not be updated",
                    uuids_new[i]);
            goto cleanup;
        }
    }

    if (testHashCheckCount(hash, count) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virHashFree(hash);
    return ret;
}


static int
testHashRemove(const void *data G_GNUC_UNUSED)
{
    int count = G_N_ELEMENTS(uuids) - G_N_ELEMENTS(uuids_subset);
    virHashTablePtr hash;
    size_t i;
    int ret = -1;

    if (!(hash = testHashInit()))
        return -1;

    for (i = 0; i < G_N_ELEMENTS(uuids_subset); i++) {
        if (virHashRemoveEntry(hash, uuids_subset[i]) < 0) {
            VIR_TEST_VERBOSE("\nentry \"%s\" could not be removed",
                    uuids_subset[i]);
            goto cleanup;
        }
    }

    if (testHashCheckCount(hash, count) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virHashFree(hash);
    return ret;
}


const int testHashCountRemoveForEachSome =
    G_N_ELEMENTS(uuids) - G_N_ELEMENTS(uuids_subset);

static int
testHashRemoveForEachSome(void *payload G_GNUC_UNUSED,
                          const char *name,
                          void *data)
{
    virHashTablePtr hash = data;
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(uuids_subset); i++) {
        if (STREQ(uuids_subset[i], name)) {
            if (virHashRemoveEntry(hash, name) < 0) {
                VIR_TEST_VERBOSE("\nentry \"%s\" could not be removed",
                        uuids_subset[i]);
            }
            break;
        }
    }
    return 0;
}


const int testHashCountRemoveForEachAll = 0;

static int
testHashRemoveForEachAll(void *payload G_GNUC_UNUSED,
                         const char *name,
                         void *data)
{
    virHashTablePtr hash = data;

    virHashRemoveEntry(hash, name);
    return 0;
}


static int
testHashRemoveForEach(const void *data)
{
    const struct testInfo *info = data;
    virHashTablePtr hash;
    int ret = -1;

    if (!(hash = testHashInit()))
        return -1;

    if (virHashForEachSafe(hash, (virHashIterator) info->data, hash)) {
        VIR_TEST_VERBOSE("\nvirHashForEach didn't go through all entries");
        goto cleanup;
    }

    if (testHashCheckCount(hash, info->count) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virHashFree(hash);
    return ret;
}


static int
testHashSteal(const void *data G_GNUC_UNUSED)
{
    int count = G_N_ELEMENTS(uuids) - G_N_ELEMENTS(uuids_subset);
    virHashTablePtr hash;
    size_t i;
    int ret = -1;

    if (!(hash = testHashInit()))
        return -1;

    for (i = 0; i < G_N_ELEMENTS(uuids_subset); i++) {
        if (!virHashSteal(hash, uuids_subset[i])) {
            VIR_TEST_VERBOSE("\nentry \"%s\" could not be stolen",
                    uuids_subset[i]);
            goto cleanup;
        }
    }

    if (testHashCheckCount(hash, count) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virHashFree(hash);
    return ret;
}


static int
testHashRemoveSetIter(const void *payload G_GNUC_UNUSED,
                      const char *name,
                      const void *data)
{
    int *count = (int *) data;
    bool rem = false;
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(uuids_subset); i++) {
        if (STREQ(uuids_subset[i], name)) {
            rem = true;
            break;
        }
    }

    if (rem || rand() % 2) {
        (*count)++;
        return 1;
    } else {
        return 0;
    }
}

static int
testHashRemoveSet(const void *data G_GNUC_UNUSED)
{
    virHashTablePtr hash;
    int count = 0;
    int rcount;
    int ret = -1;

    if (!(hash = testHashInit()))
        return -1;

    /* seed the generator so that rand() provides reproducible sequence */
    srand(9000);

    rcount = virHashRemoveSet(hash, testHashRemoveSetIter, &count);

    if (count != rcount) {
        VIR_TEST_VERBOSE("\nvirHashRemoveSet didn't remove expected number of"
                  " entries, %d != %d",
                  rcount, count);
        goto cleanup;
    }

    if (testHashCheckCount(hash, G_N_ELEMENTS(uuids) - count) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virHashFree(hash);
    return ret;
}


const int testSearchIndex = G_N_ELEMENTS(uuids_subset) / 2;

static int
testHashSearchIter(const void *payload G_GNUC_UNUSED,
                   const char *name,
                   const void *data G_GNUC_UNUSED)
{
    return STREQ(uuids_subset[testSearchIndex], name);
}

static int
testHashSearch(const void *data G_GNUC_UNUSED)
{
    virHashTablePtr hash;
    void *entry;
    int ret = -1;

    if (!(hash = testHashInit()))
        return -1;

    entry = virHashSearch(hash, testHashSearchIter, NULL, NULL);

    if (!entry || STRNEQ(uuids_subset[testSearchIndex], entry)) {
        VIR_TEST_VERBOSE("\nvirHashSearch didn't find entry '%s'",
                  uuids_subset[testSearchIndex]);
        goto cleanup;
    }

    if (testHashCheckCount(hash, G_N_ELEMENTS(uuids)) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virHashFree(hash);
    return ret;
}


static int
testHashGetItems(const void *data G_GNUC_UNUSED)
{
    virHashTablePtr hash;
    virHashKeyValuePairPtr array = NULL;
    int ret = -1;
    char keya[] = "a";
    char keyb[] = "b";
    char keyc[] = "c";
    char value1[] = "1";
    char value2[] = "2";
    char value3[] = "3";

    if (!(hash = virHashNew(NULL)) ||
        virHashAddEntry(hash, keya, value3) < 0 ||
        virHashAddEntry(hash, keyc, value1) < 0 ||
        virHashAddEntry(hash, keyb, value2) < 0) {
        VIR_TEST_VERBOSE("\nfailed to create hash");
        goto cleanup;
    }

    if (!(array = virHashGetItems(hash, NULL, false)) ||
        array[3].key || array[3].value) {
        VIR_TEST_VERBOSE("\nfailed to get items with NULL sort");
        goto cleanup;
    }
    VIR_FREE(array);

    if (!(array = virHashGetItems(hash, NULL, true)) ||
        STRNEQ(array[0].key, "a") ||
        STRNEQ(array[0].value, "3") ||
        STRNEQ(array[1].key, "b") ||
        STRNEQ(array[1].value, "2") ||
        STRNEQ(array[2].key, "c") ||
        STRNEQ(array[2].value, "1") ||
        array[3].key || array[3].value) {
        VIR_TEST_VERBOSE("\nfailed to get items with key sort");
        goto cleanup;
    }
    VIR_FREE(array);

    ret = 0;

 cleanup:
    VIR_FREE(array);
    virHashFree(hash);
    return ret;
}

static int
testHashEqualCompValue(const void *value1, const void *value2)
{
    return g_ascii_strcasecmp(value1, value2);
}

static int
testHashEqual(const void *data G_GNUC_UNUSED)
{
    virHashTablePtr hash1, hash2 = NULL;
    int ret = -1;
    char keya[] = "a";
    char keyb[] = "b";
    char keyc[] = "c";
    char value1_l[] = "m";
    char value2_l[] = "n";
    char value3_l[] = "o";
    char value1_u[] = "M";
    char value2_u[] = "N";
    char value3_u[] = "O";
    char value4_u[] = "P";

    if (!(hash1 = virHashNew(NULL)) ||
        !(hash2 = virHashNew(NULL)) ||
        virHashAddEntry(hash1, keya, value1_l) < 0 ||
        virHashAddEntry(hash1, keyb, value2_l) < 0 ||
        virHashAddEntry(hash1, keyc, value3_l) < 0 ||
        virHashAddEntry(hash2, keya, value1_u) < 0 ||
        virHashAddEntry(hash2, keyb, value2_u) < 0) {
        VIR_TEST_VERBOSE("\nfailed to create hashes");
        goto cleanup;
    }

    if (virHashEqual(hash1, hash2, testHashEqualCompValue)) {
        VIR_TEST_VERBOSE("\nfailed equal test for different number of elements");
        goto cleanup;
    }

    if (virHashAddEntry(hash2, keyc, value4_u) < 0) {
        VIR_TEST_VERBOSE("\nfailed to add element to hash2");
        goto cleanup;
    }

    if (virHashEqual(hash1, hash2, testHashEqualCompValue)) {
        VIR_TEST_VERBOSE("\nfailed equal test for same number of elements");
        goto cleanup;
    }

    if (virHashUpdateEntry(hash2, keyc, value3_u) < 0) {
        VIR_TEST_VERBOSE("\nfailed to update element in hash2");
        goto cleanup;
    }

    if (!virHashEqual(hash1, hash2, testHashEqualCompValue)) {
        VIR_TEST_VERBOSE("\nfailed equal test for equal hash tables");
        goto cleanup;
    }

    ret = 0;

 cleanup:
    virHashFree(hash1);
    virHashFree(hash2);
    return ret;
}


static int
testHashDuplicate(const void *data G_GNUC_UNUSED)
{
    g_autoptr(virHashTable) hash = NULL;

    if (!(hash = virHashNew(NULL)))
        return -1;

    if (virHashAddEntry(hash, "a", NULL) < 0) {
        VIR_TEST_VERBOSE("\nfailed to add key 'a' to hash");
        return -1;
    }

    if (virHashAddEntry(hash, "a", NULL) >= 0) {
        VIR_TEST_VERBOSE("\nadding of key 'a' should have failed");
        return -1;
    }

    return 0;
}


static int
mymain(void)
{
    int ret = 0;

#define DO_TEST_FULL(name, cmd, data, count) \
    do { \
        struct testInfo info = { data, count }; \
        if (virTestRun(name, testHash ## cmd, &info) < 0) \
            ret = -1; \
    } while (0)

#define DO_TEST_DATA(name, cmd, data) \
    DO_TEST_FULL(name "(" #data ")", \
                 cmd, \
                 testHash ## cmd ## data, \
                 testHashCount ## cmd ## data)

#define DO_TEST(name, cmd) \
    DO_TEST_FULL(name, cmd, NULL, -1)

    DO_TEST("Grow", Grow);
    DO_TEST("Update", Update);
    DO_TEST("Remove", Remove);
    DO_TEST_DATA("Remove in ForEach", RemoveForEach, Some);
    DO_TEST_DATA("Remove in ForEach", RemoveForEach, All);
    DO_TEST("Steal", Steal);
    DO_TEST("RemoveSet", RemoveSet);
    DO_TEST("Search", Search);
    DO_TEST("GetItems", GetItems);
    DO_TEST("Equal", Equal);
    DO_TEST("Duplicate entry", Duplicate);

    return (ret == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

VIR_TEST_MAIN(mymain)
