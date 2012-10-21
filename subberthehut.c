#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

#include <xmlrpc-c/base.h>
#include <xmlrpc-c/client.h>
#include <glib.h>
#include <zlib.h>

#define NAME                "subberthehut"
#define VERSION             "0.2"

#define XMLRPC_URL          "http://api.opensubtitles.org/xml-rpc"
#define LOGIN_LANGCODE      "en"
#define LOGIN_USER_AGENT    NAME

#define ZLIB_CHUNK          64 * 1024

static xmlrpc_env env;

// options default values
static const char *lang = "eng";
static bool force_overwrite = false;
static bool always_ask = false;
static bool never_ask = false;
static bool hash_search_only = false;
static bool same_name = false;

/*
 * creates the 64-bit hash used for the search query.
 * copied and modified from:
 * http://trac.opensubtitles.org/projects/opensubtitles/wiki/HashSourceCodes
 */
static unsigned long long compute_hash(FILE *handle)
{
	unsigned long long hash, fsize, tmp, i;

	fseek(handle, 0, SEEK_END);
	fsize = ftell(handle);
	fseek(handle, 0, SEEK_SET);

	hash = fsize;

	for (tmp = 0, i = 0; i < 65536 / sizeof(tmp) && fread((char *) &tmp, sizeof(tmp), 1, handle); hash += tmp, i++);
	fseek(handle, (fsize - 65536) > 0 ? (fsize - 65536) : 0, SEEK_SET);
	for (tmp = 0, i = 0; i < 65536 / sizeof(tmp) && fread((char *) &tmp, sizeof(tmp), 1, handle); hash += tmp, i++);

	return hash;
}

static int login(const char **token)
{
	xmlrpc_value *result;
	xmlrpc_value *token_xmlval;
	int r = 0;

	result = xmlrpc_client_call(&env, XMLRPC_URL, "LogIn", "(ssss)", "", "", LOGIN_LANGCODE, LOGIN_USER_AGENT);
	if (env.fault_occurred) {
		fprintf(stderr, "login failed: %s (%d)\n", env.fault_string, env.fault_code);
		r = env.fault_code;
		goto err_result;
	}

	xmlrpc_struct_find_value(&env, result, "token", &token_xmlval);
	xmlrpc_read_string(&env, token_xmlval, token);

	xmlrpc_DECREF(result);
	xmlrpc_DECREF(token_xmlval);

      err_result:
	return r;
}

/*
 * convenience function the get a string value from a xmlrpc struct.
 */
static const char *struct_get_string(xmlrpc_value *s, const char *key)
{
	xmlrpc_value *xmlval;
	const char *str;

	xmlrpc_struct_find_value(&env, s, key, &xmlval);
	xmlrpc_read_string(&env, xmlval, &str);

	xmlrpc_DECREF(xmlval);

	return str;
}

static int search_get_results(const char *token, unsigned long long hash, int filesize,
                              const char *lang, const char *filename, xmlrpc_value **data)
{
	xmlrpc_value *query1;	// hash-based query
	xmlrpc_value *sublanguageid_xmlval;
	xmlrpc_value *hash_xmlval;
	xmlrpc_value *filesize_xmlval;
	char hash_str[16 + 1];
	char filesize_str[100];

	xmlrpc_value *query2 = NULL;	// full-text query
	xmlrpc_value *filename_xmlval = NULL;

	xmlrpc_value *query_array;
	xmlrpc_value *result;

	int r = 0;

	query_array = xmlrpc_array_new(&env);

	// create hash-based query
	query1 = xmlrpc_struct_new(&env);
	sublanguageid_xmlval = xmlrpc_string_new(&env, lang);
	xmlrpc_struct_set_value(&env, query1, "sublanguageid", sublanguageid_xmlval);

	sprintf(hash_str, "%llx", hash);
	hash_xmlval = xmlrpc_string_new(&env, hash_str);
	xmlrpc_struct_set_value(&env, query1, "moviehash", hash_xmlval);

	sprintf(filesize_str, "%i", filesize);
	filesize_xmlval = xmlrpc_string_new(&env, filesize_str);
	xmlrpc_struct_set_value(&env, query1, "moviebytesize", filesize_xmlval);
	xmlrpc_array_append_item(&env, query_array, query1);

	// create full-text query
	if (!hash_search_only) {
		query2 = xmlrpc_struct_new(&env);

		xmlrpc_struct_set_value(&env, query2, "sublanguageid", sublanguageid_xmlval);

		filename_xmlval = xmlrpc_string_new(&env, filename);
		xmlrpc_struct_set_value(&env, query2, "query", filename_xmlval);

		xmlrpc_array_append_item(&env, query_array, query2);
	}

	result = xmlrpc_client_call(&env, XMLRPC_URL, "SearchSubtitles", "(sA)", token, query_array);
	if (env.fault_occurred) {
		fprintf(stderr, "query failed: %s (%d)\n", env.fault_string, env.fault_code);
		r = env.fault_code;
		goto err_result;
	}

	xmlrpc_struct_read_value(&env, result, "data", data);
	if (env.fault_occurred) {
		fprintf(stderr, "failed to get data: %s (%d)\n", env.fault_string, env.fault_code);
		r = env.fault_code;
		goto err_data;
	}

	// cleanup
      err_data:
	xmlrpc_DECREF(result);

      err_result:
	xmlrpc_DECREF(query1);
	xmlrpc_DECREF(sublanguageid_xmlval);
	xmlrpc_DECREF(hash_xmlval);
	xmlrpc_DECREF(filesize_xmlval);
	xmlrpc_DECREF(query_array);

	if (!hash_search_only) {
		xmlrpc_DECREF(query2);
		xmlrpc_DECREF(filename_xmlval);
	}

	return r;
}

static int choose_from_results(xmlrpc_value *results, int *sub_id, const char **sub_filename)
{
	struct sub_info {
		int id;
		const char *filename;
	};

	int n = xmlrpc_array_size(&env, results);
	if (env.fault_occurred) {
		fprintf(stderr, "failed to get array size: %s (%d)\n", env.fault_string, env.fault_code);
		return env.fault_code;
	}

	struct sub_info sub_infos[n];

	int sel = 0;
	for (int i = 0; i < n; i++) {
		xmlrpc_value *oneresult;
		xmlrpc_array_read_item(&env, results, i, &oneresult);

		// dear OpenSubtitles.org, why are these IDs provided as strings?
		const char *sub_id_str = struct_get_string(oneresult, "IDSubtitleFile");
		const char *lang = struct_get_string(oneresult, "SubLanguageID");
		const char *release_name = struct_get_string(oneresult, "MovieReleaseName");
		const char *sub_filename = struct_get_string(oneresult, "SubFileName");
		const char *matched_by = struct_get_string(oneresult, "MatchedBy");

		// TODO : check strtol
		sub_infos[i].id = strtol(sub_id_str, NULL, 10);
		sub_infos[i].filename = sub_filename;

		bool matched_by_moviehash = strcmp(matched_by, "moviehash") == 0;

		if (matched_by_moviehash && sel == 0)
			sel = i + 1;

		char matched_by_short = matched_by_moviehash ? 'H' : 'F';

		printf("%i. [%c] [%s] %s\t%s\n", i + 1, matched_by_short, lang, release_name, sub_filename);

		xmlrpc_DECREF(oneresult);
		free((void *) sub_id_str);
		free((void *) lang);
		free((void *) release_name);
		free((void *) matched_by);
		// sub_filename is free()d later
	}

	if (sel == 0 || always_ask) {
		if (never_ask) {
			sel = 1;
		} else {
			int sf_r;
			do {
				printf("Choose subtitle [1..%i]: ", n);
				sf_r = scanf("%i", &sel);
			} while (sf_r != 1 || sel < 1 || sel > n);
		}
	}

	*sub_id = sub_infos[sel - 1].id;
	*sub_filename = strdup(sub_infos[sel - 1].filename);

	for (int i = 0; i < n; i++)
		free((void *) sub_infos[i].filename);

	return 0;
}

static int sub_download(const char *token, int sub_id, const char *file_path)
{
	xmlrpc_value *sub_id_xmlval;
	xmlrpc_value *query_array;
	xmlrpc_value *result;

	xmlrpc_value *data;	  // result -> data
	xmlrpc_value *data_0;	  // result -> data[0]
	xmlrpc_value *data_0_sub; // result -> data[0][data]

	const char *sub_base64;	  // the subtitle, gzipped and base64 encoded

	// zlib stuff, see also http://zlib.net/zlib_how.html
	int z_ret;
	z_stream z_strm;
	unsigned char z_out[ZLIB_CHUNK];
	unsigned char z_in[ZLIB_CHUNK];
	z_strm.zalloc = Z_NULL;
	z_strm.zfree = Z_NULL;
	z_strm.opaque = Z_NULL;
	z_strm.avail_in = 0;
	z_strm.next_in = Z_NULL;

	FILE *f;
	int r = 0;

	// check if file already exists
	if (access(file_path, F_OK) == 0) {
		if (force_overwrite) {
			printf("file already exists, overwriting.\n");
		} else {
			fprintf(stderr, "file already exists, aborting. Use -f to force an overwrite.\n");
			r = errno;
			goto err_file_exists;
		}
	}

	// download
	sub_id_xmlval = xmlrpc_int_new(&env, sub_id);

	query_array = xmlrpc_array_new(&env);
	xmlrpc_array_append_item(&env, query_array, sub_id_xmlval);

	result = xmlrpc_client_call(&env, XMLRPC_URL, "DownloadSubtitles", "(sA)", token, query_array);
	if (env.fault_occurred) {
		fprintf(stderr, "query failed: %s (%d)\n", env.fault_string, env.fault_code);
		r = env.fault_code;
		goto err_result;
	}

	// get base64 encoded data
	xmlrpc_struct_find_value(&env, result, "data", &data);
	xmlrpc_array_read_item(&env, data, 0, &data_0);
	xmlrpc_struct_find_value(&env, data_0, "data", &data_0_sub);
	xmlrpc_read_string(&env, data_0_sub, &sub_base64);

	// decode and decompress to file
	f = fopen(file_path, "w+");
	if (!f) {
		perror("failed to open output file");
		r = errno;
		goto err_file;
	}

	// 16+MAX_WBITS is needed for gzip support
	z_ret = inflateInit2(&z_strm, 16 + MAX_WBITS);
	if (z_ret != Z_OK) {
		r = z_ret;
		goto err_zlib_init;
	}
	int b64_state = 0;
	unsigned int b64_save = 0;
	do {
		// write decoded data to z_in
		z_strm.avail_in = g_base64_decode_step(sub_base64, ZLIB_CHUNK, z_in, &b64_state, &b64_save);
		if (z_strm.avail_in == 0)
			break;

		z_strm.next_in = z_in;

		// decompress decoded data from z_in to z_out
		do {
			z_strm.avail_out = ZLIB_CHUNK;
			z_strm.next_out = z_out;
			z_ret = inflate(&z_strm, Z_NO_FLUSH);

			switch (z_ret) {
			case Z_NEED_DICT:
				z_ret = Z_DATA_ERROR;
			case Z_DATA_ERROR:
			case Z_MEM_ERROR:
				r = z_ret;
				fprintf(stderr, "zlib error: %s (%d)\n", z_strm.msg, z_ret);
				goto err_inflate;
			}
			// write decompressed data from z_out to file
			unsigned int have = ZLIB_CHUNK - z_strm.avail_out;
			fwrite(z_out, 1, have, f);
		} while (z_strm.avail_out == 0);
	} while (z_ret != Z_STREAM_END);

	// cleanup
      err_inflate:
	inflateEnd(&z_strm);

      err_zlib_init:
	fclose(f);

      err_file:
	xmlrpc_DECREF(result);
	xmlrpc_DECREF(data);
	xmlrpc_DECREF(data_0);
	xmlrpc_DECREF(data_0_sub);
	free((void *) sub_base64);

      err_result:
	xmlrpc_DECREF(sub_id_xmlval);
	xmlrpc_DECREF(query_array);

      err_file_exists:
	return r;
}

static void usage()
{
	printf("usage: %s [options] <file>\n\n", NAME);

	puts("OpenSubtitles.org downloader.\n");

	printf("%s can do a hash-based and a fulltext-based search.\n"
	       "On a hash-based search, %s will generate a hash from the specified\n"
	       "video file and use this to search for appropriate subtitles.\n"
	       "Any results from this hash-based search are definitively compatible\n"
	       "with the video file, therefore %s will, by default, automatically\n"
	       "download the first subtitle from these search results.\n"
	       "In case the hash-based search returns no results, %s will also\n"
	       "do a fulltext-based search, meaning the OpenSubtitle.org database\n"
	       "will be searched with the filename of the specified file. The results\n"
	       "from this search are not guaranteed to be compatible with the video\n"
	       "file, therefore %s will, by default, ask the user which subtitle to\n"
	       "download.\n\n",
	       NAME, NAME, NAME, NAME, NAME);

	puts("Options:\n"
	     " -h, --help              Display help and exit.\n"
	     " -l, --lang <languages>  Comma-separated list of languages to search for,\n"
	     "                         e.g. 'eng,ger'. Default is 'eng'.\n"
	     " -a, --always-ask        Always ask which subtitle to download, even\n"
	     "                         when there are hash-based results.\n"
	     " -n, --never-ask         Never ask which subtitle to download, even\n"
	     "                         when there are only filename based results.\n"
	     "                         When this option is specified, the first\n"
	     "                         search result will be downloaded.\n"
	     " -f, --force             Overwrite output file if it already exists.\n"
	     " -o, --hash-search-only  Only do a hash-based search.\n"
	     " -s, --same-name         Download the subtitle to the same filename as the\n"
	     "                         original file, only replacing the file extension.\n");
}

static const char *get_sub_path(const char *filepath, const char *sub_filename)
{
	char *sub_filepath;

	if (same_name) {
		const char *sub_ext = strrchr(sub_filename, '.');
		if (sub_ext == NULL) {
			fprintf(stderr, "warning: subtitle filename from the OpenSubtitles.org "
			                "database has no file extension, assuming .srt.\n");
			sub_ext = ".srt";
		}
		const char *lastdot = strrchr(filepath, '.');
		int index;
		if (lastdot == NULL)
			index = strlen(filepath) - 1;
		else
			index = (lastdot - filepath);

		sub_filepath = malloc(index + 1 + strlen(sub_ext) + 1);
		strncpy(sub_filepath, filepath, index);
		sub_filepath[index] = '\0';
		strcat(sub_filepath, sub_ext);

	} else {
		const char *lastslash = strrchr(filepath, '/');
		if (lastslash == NULL) {
			sub_filepath = strdup(sub_filename);
		} else {
			int index = (lastslash - filepath);
			sub_filepath = malloc(index + 1 + strlen(sub_filename) + 1);
			strncpy(sub_filepath, filepath, index + 1);
			sub_filepath[index + 1] = '\0';
			strcat(sub_filepath, sub_filename);
		}
	}
	return sub_filepath;
}

int main(int argc, char *argv[])
{
	const char *filepath;
	const char *token;

	FILE *f;
	unsigned long long hash;
	int filesize;

	xmlrpc_value *results;

	const char *sub_filename = NULL;
	const char *sub_filepath;

	int r = EXIT_SUCCESS;

	// parse options
	const struct option opts[] = {
		{"help", no_argument, NULL, 'h'},
		{"lang", required_argument, NULL, 'l'},
		{"always-ask", no_argument, NULL, 'a'},
		{"never-ask", no_argument, NULL, 'n'},
		{"force", no_argument, NULL, 'f'},
		{"hash-search-only", no_argument, NULL, 'o'},
		{"same-name", no_argument, NULL, 's'}
	};

	int c;
	while ((c = getopt_long(argc, argv, "hl:anfos", opts, NULL)) != -1) {
		switch (c) {
		case 'h':
			usage();
			return EXIT_SUCCESS;

		case 'l':
			lang = optarg;
			break;

		case 'a':
			always_ask = true;
			break;

		case 'n':
			never_ask = true;
			break;

		case 'f':
			force_overwrite = true;
			break;

		case 'o':
			hash_search_only = true;
			break;

		case 's':
			same_name = true;
			break;

		default:
			return EXIT_FAILURE;
		}

	}

	// check if user has specified exactly one file
	if (argc - optind != 1) {
		usage();
		return EXIT_FAILURE;
	}
	// get hash/filesize
	filepath = argv[optind];
	f = fopen(filepath, "r");
	if (!f) {
		perror("failed to open file");
		return errno;
	}

	hash = compute_hash(f);

	fseek(f, 0, SEEK_END);
	filesize = ftell(f);

	// xmlrpc init
	xmlrpc_env_init(&env);

	xmlrpc_client_init2(&env, XMLRPC_CLIENT_NO_FLAGS, NAME, VERSION, NULL, 0);
	if (env.fault_occurred) {
		fprintf(stderr, "failed to init xmlrpc client: %s (%d)\n", env.fault_string, env.fault_code);
		r = env.fault_code;
		goto err_xmlrpc_init;
	}
	// login
	r = login(&token);
	if (r != 0)
		goto err_login;

	// start search
	printf("searching...\n");

	const char *filename = strrchr(filepath, '/');
	if (filename == NULL)
		filename = filepath;

	r = search_get_results(token, hash, filesize, lang, filename, &results);
	if (r != 0) {
		goto err_results;
	}
	// for some reason [data] is of type XMLRPC_TYPE_BOOL if the search returns no hits!?
	if (xmlrpc_value_type(results) != XMLRPC_TYPE_ARRAY) {
		printf("no results.\n");
		r = EXIT_FAILURE;
		goto err_noresults;
	}
	// let user choose the subtitle to download
	int sub_id = 0;
	r = choose_from_results(results, &sub_id, &sub_filename);
	if (r != 0)
		goto err_choose;

	sub_filepath = get_sub_path(filepath, sub_filename);

	printf("downloading to %s ...\n", sub_filepath);
	r = sub_download(token, sub_id, sub_filepath);

	free((void *) sub_filepath);
	fclose(f);

      err_choose:
	free((void *) sub_filename);

      err_noresults:
	xmlrpc_DECREF(results);

      err_results:
	free((void *) token);

      err_login:
	xmlrpc_client_cleanup();

      err_xmlrpc_init:
	xmlrpc_env_clean(&env);
	return r;
}
