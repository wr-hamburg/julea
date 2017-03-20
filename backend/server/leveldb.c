/*
 * JULEA - Flexible storage framework
 * Copyright (C) 2017 Michael Kuhn
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _POSIX_C_SOURCE 200809L

#include <julea-config.h>

#include <glib.h>
#include <gmodule.h>

#include <leveldb/c.h>

#include <jbackend.h>
#include <jsemantics.h>

struct JLevelDBBatch
{
	leveldb_writebatch_t* batch;
	gchar* namespace;
};

typedef struct JLevelDBBatch JLevelDBBatch;

struct JLevelDBIterator
{
	leveldb_iterator_t* iterator;
	gchar* prefix;
};

typedef struct JLevelDBIterator JLevelDBIterator;

static leveldb_t* backend_db = NULL;

static leveldb_readoptions_t* backend_read_options = NULL;
static leveldb_writeoptions_t* backend_write_options = NULL;

static
gboolean
backend_batch_start (gchar const* namespace, gpointer* data)
{
	JLevelDBBatch* batch;

	g_return_val_if_fail(namespace != NULL, FALSE);
	g_return_val_if_fail(data != NULL, FALSE);

	batch = g_slice_new(JLevelDBBatch);

	batch->batch = leveldb_writebatch_create();
	batch->namespace = g_strdup(namespace);
	*data = batch;

	return (batch != NULL);
}

static
gboolean
backend_batch_execute (gpointer data)
{
	JLevelDBBatch* batch = data;

	g_return_val_if_fail(data != NULL, FALSE);

	leveldb_write(backend_db, backend_write_options, batch->batch, NULL);

	g_free(batch->namespace);
	leveldb_writebatch_destroy(batch->batch);
	g_slice_free(JLevelDBBatch, batch);

	// FIXME
	return TRUE;
}

static
gboolean
backend_put (gpointer data, gchar const* key, bson_t const* value)
{
	JLevelDBBatch* batch = data;
	gchar* nskey;

	g_return_val_if_fail(key != NULL, FALSE);
	g_return_val_if_fail(value != NULL, FALSE);
	g_return_val_if_fail(data != NULL, FALSE);

	nskey = g_strdup_printf("%s:%s", batch->namespace, key);
	leveldb_writebatch_put(batch->batch, nskey, strlen(nskey) + 1, (gchar*)bson_get_data(value), value->len);
	g_free(nskey);

	// FIXME
	return TRUE;
}

static
gboolean
backend_delete (gpointer data, gchar const* key)
{
	JLevelDBBatch* batch = data;
	gchar* nskey;

	g_return_val_if_fail(key != NULL, FALSE);
	g_return_val_if_fail(data != NULL, FALSE);

	nskey = g_strdup_printf("%s:%s", batch->namespace, key);
	leveldb_writebatch_delete(batch->batch, nskey, strlen(nskey) + 1);
	g_free(nskey);

	// FIXME
	return TRUE;
}

static
gboolean
backend_get (gchar const* namespace, gchar const* key, bson_t* result_out)
{
	gchar* nskey;
	gpointer result;
	gsize result_len;

	g_return_val_if_fail(namespace != NULL, FALSE);
	g_return_val_if_fail(key != NULL, FALSE);
	g_return_val_if_fail(result_out != NULL, FALSE);

	nskey = g_strdup_printf("%s:%s", namespace, key);
	result = leveldb_get(backend_db, backend_read_options, nskey, strlen(nskey) + 1, &result_len, NULL);
	g_free(nskey);

	if (result != NULL)
	{
		bson_t tmp[1];

		// FIXME check whether copies can be avoided
		bson_init_static(tmp, result, result_len);
		bson_copy_to(tmp, result_out);
		g_free(result);
	}

	return (result != NULL);
}

static
gboolean
backend_get_all (gchar const* namespace, gpointer* data)
{
	JLevelDBIterator* iterator = NULL;
	leveldb_iterator_t* it;

	g_return_val_if_fail(namespace != NULL, FALSE);
	g_return_val_if_fail(data != NULL, FALSE);

	it = leveldb_create_iterator(backend_db, backend_read_options);

	if (it != NULL)
	{
		iterator = g_slice_new(JLevelDBIterator);
		iterator->iterator = it;
		iterator->prefix = g_strdup_printf("%s:", namespace);

		// FIXME check +1
		leveldb_iter_seek(iterator->iterator, iterator->prefix, strlen(iterator->prefix) + 1);

		*data = iterator;
	}

	return (iterator != NULL);
}

static
gboolean
backend_get_by_value (gchar const* namespace, bson_t const* value, gpointer* data)
{
	gboolean ret = FALSE;

	g_return_val_if_fail(namespace != NULL, FALSE);
	g_return_val_if_fail(value != NULL, FALSE);
	g_return_val_if_fail(data != NULL, FALSE);

	return ret;
}

static
gboolean
backend_iterate (gpointer data, bson_t* result_out)
{
	gboolean ret = FALSE;

	JLevelDBIterator* iterator = data;

	g_return_val_if_fail(data != NULL, FALSE);
	g_return_val_if_fail(result_out != NULL, FALSE);

	if (leveldb_iter_valid(iterator->iterator))
	{
		gchar const* key;
		gconstpointer value;
		gsize len;

		key = leveldb_iter_key(iterator->iterator, &len);

		if (!g_str_has_prefix(key, iterator->prefix))
		{
			// FIXME check whether we can completely terminate
			goto out;
		}

		value = leveldb_iter_value(iterator->iterator, &len);
		bson_init_static(result_out, value, len);

		// FIXME might invalidate value
		leveldb_iter_next(iterator->iterator);

		ret = TRUE;
	}
	else
	{
		g_free(iterator->prefix);
		leveldb_iter_destroy(iterator->iterator);
		g_slice_free(JLevelDBIterator, iterator);
	}

out:
	return ret;
}

static
gboolean
backend_init (gchar const* path)
{
	leveldb_options_t* options;

	g_return_val_if_fail(path != NULL, FALSE);

	options = leveldb_options_create();
	leveldb_options_set_create_if_missing(options, 1);
	leveldb_options_set_compression(options, leveldb_snappy_compression);

	backend_read_options = leveldb_readoptions_create();
	backend_write_options = leveldb_writeoptions_create();

	backend_db = leveldb_open(options, path, NULL);

	leveldb_options_destroy(options);

	return (backend_db != NULL);
}

static
void
backend_fini (void)
{
	leveldb_readoptions_destroy(backend_read_options);
	leveldb_writeoptions_destroy(backend_write_options);

	if (backend_db != NULL)
	{
		leveldb_close(backend_db);
	}
}

static
JBackend leveldb_backend = {
	.type = J_BACKEND_TYPE_META,
	.meta = {
		.init = backend_init,
		.fini = backend_fini,
		.batch_start = backend_batch_start,
		.batch_execute = backend_batch_execute,
		.put = backend_put,
		.delete = backend_delete,
		.get = backend_get,
		.get_all = backend_get_all,
		.get_by_value = backend_get_by_value,
		.iterate = backend_iterate
	}
};

G_MODULE_EXPORT
JBackend*
backend_info (JBackendType type)
{
	JBackend* backend = NULL;

	if (type == J_BACKEND_TYPE_META)
	{
		backend = &leveldb_backend;
	}

	return backend;
}