#include "cache.h"
#include "config.h"
#include "dir.h"
#include "ewah/ewok.h"
#include "fsmonitor.h"
#include "fsmonitor-ipc.h"
#include "run-command.h"
#include "strbuf.h"

#define INDEX_EXTENSION_VERSION1	(1)
#define INDEX_EXTENSION_VERSION2	(2)
#define HOOK_INTERFACE_VERSION1		(1)
#define HOOK_INTERFACE_VERSION2		(2)

struct trace_key trace_fsmonitor = TRACE_KEY_INIT(FSMONITOR);

static void assert_index_minimum(struct index_state *istate, size_t pos)
{
	if (pos > istate->cache_nr)
		BUG("fsmonitor_dirty has more entries than the index (%"PRIuMAX" > %u)",
		    (uintmax_t)pos, istate->cache_nr);
}

static void fsmonitor_ewah_callback(size_t pos, void *is)
{
	struct index_state *istate = (struct index_state *)is;
	struct cache_entry *ce;

	assert_index_minimum(istate, pos + 1);

	ce = istate->cache[pos];
	ce->ce_flags &= ~CE_FSMONITOR_VALID;
}

static int fsmonitor_hook_version(void)
{
	int hook_version;

	if (git_config_get_int("core.fsmonitorhookversion", &hook_version))
		return -1;

	if (hook_version == HOOK_INTERFACE_VERSION1 ||
	    hook_version == HOOK_INTERFACE_VERSION2)
		return hook_version;

	warning("Invalid hook version '%i' in core.fsmonitorhookversion. "
		"Must be 1 or 2.", hook_version);
	return -1;
}

int read_fsmonitor_extension(struct index_state *istate, const void *data,
	unsigned long sz)
{
	const char *index = data;
	uint32_t hdr_version;
	uint32_t ewah_size;
	struct ewah_bitmap *fsmonitor_dirty;
	int ret;
	uint64_t timestamp;
	struct strbuf last_update = STRBUF_INIT;

	if (sz < sizeof(uint32_t) + 1 + sizeof(uint32_t))
		return error("corrupt fsmonitor extension (too short)");

	hdr_version = get_be32(index);
	index += sizeof(uint32_t);
	if (hdr_version == INDEX_EXTENSION_VERSION1) {
		timestamp = get_be64(index);
		strbuf_addf(&last_update, "%"PRIu64"", timestamp);
		index += sizeof(uint64_t);
	} else if (hdr_version == INDEX_EXTENSION_VERSION2) {
		strbuf_addstr(&last_update, index);
		index += last_update.len + 1;
	} else {
		return error("bad fsmonitor version %d", hdr_version);
	}

	istate->fsmonitor_last_update = strbuf_detach(&last_update, NULL);

	ewah_size = get_be32(index);
	index += sizeof(uint32_t);

	fsmonitor_dirty = ewah_new();
	ret = ewah_read_mmap(fsmonitor_dirty, index, ewah_size);
	if (ret != ewah_size) {
		ewah_free(fsmonitor_dirty);
		return error("failed to parse ewah bitmap reading fsmonitor index extension");
	}
	istate->fsmonitor_dirty = fsmonitor_dirty;

	if (!istate->split_index)
		assert_index_minimum(istate, istate->fsmonitor_dirty->bit_size);

	trace2_data_string("index", NULL, "extension/fsmn/read/token",
			   istate->fsmonitor_last_update);
	trace_printf_key(&trace_fsmonitor,
			 "read fsmonitor extension successful '%s'",
			 istate->fsmonitor_last_update);
	return 0;
}

void fill_fsmonitor_bitmap(struct index_state *istate)
{
	unsigned int i, skipped = 0;
	istate->fsmonitor_dirty = ewah_new();
	for (i = 0; i < istate->cache_nr; i++) {
		if (istate->cache[i]->ce_flags & CE_REMOVE)
			skipped++;
		else if (!(istate->cache[i]->ce_flags & CE_FSMONITOR_VALID))
			ewah_set(istate->fsmonitor_dirty, i - skipped);
	}
}

void write_fsmonitor_extension(struct strbuf *sb, struct index_state *istate)
{
	uint32_t hdr_version;
	uint32_t ewah_start;
	uint32_t ewah_size = 0;
	int fixup = 0;

	if (!istate->split_index)
		assert_index_minimum(istate, istate->fsmonitor_dirty->bit_size);

	put_be32(&hdr_version, INDEX_EXTENSION_VERSION2);
	strbuf_add(sb, &hdr_version, sizeof(uint32_t));

	strbuf_addstr(sb, istate->fsmonitor_last_update);
	strbuf_addch(sb, 0); /* Want to keep a NUL */

	fixup = sb->len;
	strbuf_add(sb, &ewah_size, sizeof(uint32_t)); /* we'll fix this up later */

	ewah_start = sb->len;
	ewah_serialize_strbuf(istate->fsmonitor_dirty, sb);
	ewah_free(istate->fsmonitor_dirty);
	istate->fsmonitor_dirty = NULL;

	/* fix up size field */
	put_be32(&ewah_size, sb->len - ewah_start);
	memcpy(sb->buf + fixup, &ewah_size, sizeof(uint32_t));

	trace2_data_string("index", NULL, "extension/fsmn/write/token",
			   istate->fsmonitor_last_update);
	trace_printf_key(&trace_fsmonitor,
			 "write fsmonitor extension successful '%s'",
			 istate->fsmonitor_last_update);
}

/*
 * Call the query-fsmonitor hook passing the last update token of the saved results.
 */
static int query_fsmonitor_hook(struct repository *r,
				int version,
				const char *last_update,
				struct strbuf *query_result)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	int result;

	if (r->settings.fsmonitor_mode != FSMONITOR_MODE_HOOK)
		return -1;

	assert(r->settings.fsmonitor_hook_path);
	assert(*r->settings.fsmonitor_hook_path);

	strvec_push(&cp.args, r->settings.fsmonitor_hook_path);
	strvec_pushf(&cp.args, "%d", version);
	strvec_pushf(&cp.args, "%s", last_update);
	cp.use_shell = 1;
	cp.dir = get_git_work_tree();

	trace2_region_enter("fsm_hook", "query", NULL);

	result = capture_command(&cp, query_result, 1024);

	if (result)
		trace2_data_intmax("fsm_hook", NULL, "query/failed", result);
	else {
		trace2_data_intmax("fsm_hook", NULL, "query/response-length",
				   query_result->len);

		if (fsmonitor_is_trivial_response(query_result))
			trace2_data_intmax("fsm_hook", NULL,
					   "query/trivial-response", 1);
	}

	trace2_region_leave("fsm_hook", "query", NULL);

	return result;
}

int fsmonitor_is_trivial_response(const struct strbuf *query_result)
{
	static char trivial_response[3] = { '\0', '/', '\0' };

	return query_result->len >= 3 &&
		!memcmp(trivial_response,
			&query_result->buf[query_result->len - 3], 3);
}

static void fsmonitor_refresh_callback(struct index_state *istate, char *name)
{
	int i, len = strlen(name);
	if (name[len - 1] == '/') {

		/*
		 * TODO We should binary search to find the first path with
		 * TODO this directory prefix.  Then linearly update entries
		 * TODO while the prefix matches.  Taking care to search without
		 * TODO the trailing slash -- because '/' sorts after a few
		 * TODO interesting special chars, like '.' and ' '.
		 */

		/* Mark all entries for the folder invalid */
		for (i = 0; i < istate->cache_nr; i++) {
			if (istate->cache[i]->ce_flags & CE_FSMONITOR_VALID &&
			    starts_with(istate->cache[i]->name, name))
				istate->cache[i]->ce_flags &= ~CE_FSMONITOR_VALID;
		}
		/* Need to remove the / from the path for the untracked cache */
		name[len - 1] = '\0';
	} else {
		int pos = index_name_pos(istate, name, strlen(name));

		if (pos >= 0) {
			struct cache_entry *ce = istate->cache[pos];
			ce->ce_flags &= ~CE_FSMONITOR_VALID;
		}
	}

	/*
	 * Mark the untracked cache dirty even if it wasn't found in the index
	 * as it could be a new untracked file.
	 */
	trace_printf_key(&trace_fsmonitor, "fsmonitor_refresh_callback '%s'", name);
	untracked_cache_invalidate_path(istate, name, 0);
}

/*
 * The number of pathnames that we need to receive from FSMonitor
 * before we force the index to be updated.
 *
 * Note that any pathname within the set of received paths MAY cause
 * cache-entry or istate flag bits to be updated and thus cause the
 * index to be updated on disk.
 *
 * However, the response may contain many paths (such as ignored
 * paths) that will not update any flag bits.  And thus not force the
 * index to be updated.  (This is fine and normal.)  It also means
 * that the token will not be updated in the FSMonitor index
 * extension.  So the next Git command will find the same token in the
 * index, make the same token-relative request, and receive the same
 * response (plus any newly changed paths).  If this response is large
 * (and continues to grow), performance could be impacted.
 *
 * For example, if the user runs a build and it writes 100K object
 * files but doesn't modify any source files, the index would not need
 * to be updated.  The FSMonitor response (after the build and
 * relative to a pre-build token) might be 5MB.  Each subsequent Git
 * command will receive that same 100K/5MB response until something
 * causes the index to be updated.  And `refresh_fsmonitor()` will
 * have to iterate over those 100K paths each time.
 *
 * Performance could be improved if we optionally force update the
 * index after a very large response and get an updated token into
 * the FSMonitor index extension.  This should allow subsequent
 * commands to get smaller and more current responses.
 *
 * The value chosen here does not need to be precise.  The index
 * will be updated automatically the first time the user touches
 * a tracked file and causes a command like `git status` to
 * update an mtime to be updated and/or set a flag bit.
 *
 * NEEDSWORK: Does this need to be a config value?
 */
static int fsmonitor_force_update_threshold = 100;

void refresh_fsmonitor(struct index_state *istate)
{
	struct strbuf query_result = STRBUF_INIT;
	int query_success = 0, hook_version = -1;
	size_t bol = 0; /* beginning of line */
	uint64_t last_update;
	struct strbuf last_update_token = STRBUF_INIT;
	char *buf;
	unsigned int i;
	struct repository *r = istate->repo ? istate->repo : the_repository;

	if (r->settings.fsmonitor_mode <= FSMONITOR_MODE_DISABLED ||
	    istate->fsmonitor_has_run_once)
		return;

	istate->fsmonitor_has_run_once = 1;

	trace_printf_key(&trace_fsmonitor, "refresh fsmonitor");

	if (r->settings.fsmonitor_mode == FSMONITOR_MODE_IPC) {
		query_success = !fsmonitor_ipc__send_query(
			istate->fsmonitor_last_update ?
			istate->fsmonitor_last_update : "builtin:fake",
			&query_result);
		if (query_success) {
			/*
			 * The response contains a series of nul terminated
			 * strings.  The first is the new token.
			 *
			 * Use `char *buf` as an interlude to trick the CI
			 * static analysis to let us use `strbuf_addstr()`
			 * here (and only copy the token) rather than
			 * `strbuf_addbuf()`.
			 */
			buf = query_result.buf;
			strbuf_addstr(&last_update_token, buf);
			bol = last_update_token.len + 1;
		} else {
			/*
			 * The builtin daemon is not available on this
			 * platform -OR- we failed to get a response.
			 *
			 * Generate a fake token (rather than a V1
			 * timestamp) for the index extension.  (If
			 * they switch back to the hook API, we don't
			 * want ambiguous state.)
			 */
			strbuf_addstr(&last_update_token, "builtin:fake");
		}

		/*
		 * Regardless of whether we successfully talked to a
		 * fsmonitor daemon or not, we skip over and do not
		 * try to use the hook.  The "core.useBuiltinFSMonitor"
		 * config setting ALWAYS overrides the "core.fsmonitor"
		 * hook setting.
		 */
		goto apply_results;
	}

	assert(r->settings.fsmonitor_mode == FSMONITOR_MODE_HOOK);

	hook_version = fsmonitor_hook_version();

	/*
	 * This could be racy so save the date/time now and query_fsmonitor_hook
	 * should be inclusive to ensure we don't miss potential changes.
	 */
	last_update = getnanotime();
	if (hook_version == HOOK_INTERFACE_VERSION1)
		strbuf_addf(&last_update_token, "%"PRIu64"", last_update);

	/*
	 * If we have a last update token, call query_fsmonitor_hook for the set of
	 * changes since that token, else assume everything is possibly dirty
	 * and check it all.
	 */
	if (istate->fsmonitor_last_update) {
		if (hook_version == -1 || hook_version == HOOK_INTERFACE_VERSION2) {
			query_success = !query_fsmonitor_hook(
				r, HOOK_INTERFACE_VERSION2,
				istate->fsmonitor_last_update, &query_result);

			if (query_success) {
				if (hook_version < 0)
					hook_version = HOOK_INTERFACE_VERSION2;

				/*
				 * First entry will be the last update token
				 * Need to use a char * variable because static
				 * analysis was suggesting to use strbuf_addbuf
				 * but we don't want to copy the entire strbuf
				 * only the chars up to the first NUL
				 */
				buf = query_result.buf;
				strbuf_addstr(&last_update_token, buf);
				if (!last_update_token.len) {
					warning("Empty last update token.");
					query_success = 0;
				} else {
					bol = last_update_token.len + 1;
				}
			} else if (hook_version < 0) {
				hook_version = HOOK_INTERFACE_VERSION1;
				if (!last_update_token.len)
					strbuf_addf(&last_update_token, "%"PRIu64"", last_update);
			}
		}

		if (hook_version == HOOK_INTERFACE_VERSION1) {
			query_success = !query_fsmonitor_hook(
				r, HOOK_INTERFACE_VERSION1,
				istate->fsmonitor_last_update, &query_result);
		}

		trace_performance_since(last_update, "fsmonitor process '%s'",
					r->settings.fsmonitor_hook_path);
		trace_printf_key(&trace_fsmonitor,
				 "fsmonitor process '%s' returned %s",
				 r->settings.fsmonitor_hook_path,
				 query_success ? "success" : "failure");
	}

apply_results:
	/*
	 * The response from FSMonitor (excluding the header token) is
	 * either:
	 *
	 * [a] a (possibly empty) list of NUL delimited relative
	 *     pathnames of changed paths.  This list can contain
	 *     files and directories.  Directories have a trailing
	 *     slash.
	 *
	 * [b] a single '/' to indicate the provider had no
	 *     information and that we should consider everything
	 *     invalid.  We call this a trivial response.
	 */
	if (query_success && query_result.buf[bol] != '/') {
		/*
		 * Mark all pathnames returned by the monitor as dirty.
		 *
		 * This updates both the cache-entries and the untracked-cache.
		 */
		int count = 0;

		buf = query_result.buf;
		for (i = bol; i < query_result.len; i++) {
			if (buf[i] != '\0')
				continue;
			fsmonitor_refresh_callback(istate, buf + bol);
			bol = i + 1;
			count++;
		}
		if (bol < query_result.len) {
			fsmonitor_refresh_callback(istate, buf + bol);
			count++;
		}

		/* Now mark the untracked cache for fsmonitor usage */
		if (istate->untracked)
			istate->untracked->use_fsmonitor = 1;

		if (count > fsmonitor_force_update_threshold)
			istate->cache_changed |= FSMONITOR_CHANGED;

	} else {
		/*
		 * We received a trivial response, so invalidate everything.
		 *
		 * We only want to run the post index changed hook if
		 * we've actually changed entries, so keep track if we
		 * actually changed entries or not.
		 */
		int is_cache_changed = 0;

		for (i = 0; i < istate->cache_nr; i++) {
			if (istate->cache[i]->ce_flags & CE_FSMONITOR_VALID) {
				is_cache_changed = 1;
				istate->cache[i]->ce_flags &= ~CE_FSMONITOR_VALID;
			}
		}

		/*
		 * If we're going to check every file, ensure we save
		 * the results.
		 */
		if (is_cache_changed)
			istate->cache_changed |= FSMONITOR_CHANGED;

		if (istate->untracked)
			istate->untracked->use_fsmonitor = 0;
	}
	strbuf_release(&query_result);

	/* Now that we've updated istate, save the last_update_token */
	FREE_AND_NULL(istate->fsmonitor_last_update);
	istate->fsmonitor_last_update = strbuf_detach(&last_update_token, NULL);
}

/*
 * The caller wants to turn on FSMonitor.  And when the caller writes
 * the index to disk, a FSMonitor extension should be included.  This
 * requires that `istate->fsmonitor_last_update` not be NULL.  But we
 * have not actually talked to a FSMonitor process yet, so we don't
 * have an initial value for this field.
 *
 * For a protocol V1 FSMonitor process, this field is a formatted
 * "nanoseconds since epoch" field.  However, for a protocol V2
 * FSMonitor process, this field is an opaque token.
 *
 * Historically, `add_fsmonitor()` has initialized this field to the
 * current time for protocol V1 processes.  There are lots of race
 * conditions here, but that code has shipped...
 *
 * The only true solution is to use a V2 FSMonitor and get a current
 * or default token value (that it understands), but we cannot do that
 * until we have actually talked to an instance of the FSMonitor process
 * (but the protocol requires that we send a token first...).
 *
 * For simplicity, just initialize like we have a V1 process and require
 * that V2 processes adapt.
 */
static void initialize_fsmonitor_last_update(struct index_state *istate)
{
	struct strbuf last_update = STRBUF_INIT;

	strbuf_addf(&last_update, "%"PRIu64"", getnanotime());
	istate->fsmonitor_last_update = strbuf_detach(&last_update, NULL);
}

void add_fsmonitor(struct index_state *istate)
{
	unsigned int i;

	if (!istate->fsmonitor_last_update) {
		trace_printf_key(&trace_fsmonitor, "add fsmonitor");
		istate->cache_changed |= FSMONITOR_CHANGED;
		initialize_fsmonitor_last_update(istate);

		/* reset the fsmonitor state */
		for (i = 0; i < istate->cache_nr; i++)
			istate->cache[i]->ce_flags &= ~CE_FSMONITOR_VALID;

		/* reset the untracked cache */
		if (istate->untracked) {
			add_untracked_cache(istate);
			istate->untracked->use_fsmonitor = 1;
		}

		/* Update the fsmonitor state */
		refresh_fsmonitor(istate);
	}
}

void remove_fsmonitor(struct index_state *istate)
{
	if (istate->fsmonitor_last_update) {
		trace_printf_key(&trace_fsmonitor, "remove fsmonitor");
		istate->cache_changed |= FSMONITOR_CHANGED;
		FREE_AND_NULL(istate->fsmonitor_last_update);
	}
}

void tweak_fsmonitor(struct index_state *istate)
{
	unsigned int i;
	struct repository *r = istate->repo ? istate->repo : the_repository;
	int fsmonitor_enabled = r->settings.fsmonitor_mode > FSMONITOR_MODE_DISABLED;

	if (istate->fsmonitor_dirty) {
		if (fsmonitor_enabled) {
			/* Mark all entries valid */
			for (i = 0; i < istate->cache_nr; i++) {
				istate->cache[i]->ce_flags |= CE_FSMONITOR_VALID;
			}

			/* Mark all previously saved entries as dirty */
			assert_index_minimum(istate, istate->fsmonitor_dirty->bit_size);
			ewah_each_bit(istate->fsmonitor_dirty, fsmonitor_ewah_callback, istate);

			refresh_fsmonitor(istate);
		}

		ewah_free(istate->fsmonitor_dirty);
		istate->fsmonitor_dirty = NULL;
	}

	if (fsmonitor_enabled)
		add_fsmonitor(istate);
	else
		remove_fsmonitor(istate);
}
