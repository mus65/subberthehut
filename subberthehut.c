#define _GNU_SOURCE
#define FILE_scoped_ptr_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <inttypes.h> // uint64_t / PRIx64

#include <xmlrpc-c/base.h>
#include <xmlrpc-c/client.h>
#include <glib.h> // g_base64_decode_step
#include <zlib.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#define STH_XMLRPC_URL         "http://api.opensubtitles.org/xml-rpc"
#define LOGIN_LANGCODE         "en"
#define LOGIN_USER_AGENT       "subberthehut"

#define ZLIB_CHUNK             (64 * 1024)

#define STH_XMLRPC_SIZE_LIMIT  (10 * 1024 * 1024)

#define HEADER_ID              '#'
#define HEADER_MATCHED_BY_HASH 'H'
#define HEADER_LANG            "Lng"
#define HEADER_RELEASE_NAME    "Release / File Name"

#define SEP_VERTICAL           "\342\224\202"
#define SEP_HORIZONTAL         "\342\224\200"
#define SEP_CROSS              "\342\224\274"
#define SEP_UP_RIGHT           "\342\224\224"

/* __attribute__(cleanup) */
#define DECLARE_CLEANUP_FNC(type, function) \
    static void cleanup_ ## type (type **ptr) __attribute__((unused)); \
    static void cleanup_ ## type (type **ptr) { \
        if(*ptr) { function(*ptr); } \
    } \
    \
    static void cleanup_const_ ## type (const type **ptr) __attribute__((unused)); \
    static void cleanup_const_ ## type (const type **ptr) { \
        if(*ptr) { function(*(type**)ptr); } \
    } \

#define scoped_ptr(type) __attribute__((cleanup(cleanup_ ## type))) type *
#define scoped_cptr(type) __attribute__((cleanup(cleanup_const_ ## type))) const type *

DECLARE_CLEANUP_FNC(char, free)
DECLARE_CLEANUP_FNC(gchar, g_free)
DECLARE_CLEANUP_FNC(FILE, fclose)
DECLARE_CLEANUP_FNC(xmlrpc_value, xmlrpc_DECREF)
DECLARE_CLEANUP_FNC(GRegex, g_regex_unref)
DECLARE_CLEANUP_FNC(GMatchInfo, g_match_info_free)
DECLARE_CLEANUP_FNC(GError, g_error_free)
DECLARE_CLEANUP_FNC(GDir, g_dir_close)
DECLARE_CLEANUP_FNC(AVFormatContext, avformat_free_context)

/* end __attribute__(cleanup) */

static xmlrpc_env env;
static xmlrpc_client *client;

// options default values
static const char *lang = "eng";
static bool list_languages = false;
static bool force_overwrite = false;
static bool always_ask = false;
static bool never_ask = false;
static bool hash_search_only = false;
static bool name_search_only = false;
static bool same_name = false;
static bool upload_info = true;
static int limit = 10;
static bool exit_on_fail = false;
static unsigned int quiet = 0;

struct sub_info {
	int id;
	bool matched_by_hash;
	const char* lang;
	const char* release_name;
	const char* filename;
};

static void cleanup_sub_info(struct sub_info* ptr) {
    free((void *) ptr->lang);
    free((void *) ptr->release_name);
    free((void *) ptr->filename);
}

//////////////////////////////////////////////////////////////////////////////////////////////////
static void log_err(const char *format, ...) {
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	putc('\n', stderr);

}

//////////////////////////////////////////////////////////////////////////////////////////////////
static void log_info(const char *format, ...) {
	if (quiet >= 2)
		return;
    
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	putchar('\n');
}

//////////////////////////////////////////////////////////////////////////////////////////////////
static int log_oom() {
	log_err("Out of Memory.");
	return ENOMEM;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
/// creates the 64-bit hash used for the search query.
/// copied and modified from:
/// http://trac.opensubtitles.org/projects/opensubtitles/wiki/HashSourceCodes
static void get_hash_and_filesize(FILE *handle, uint64_t *hash, uint64_t *filesize) {
	fseek(handle, 0, SEEK_END);
	*filesize = ftell(handle);
	fseek(handle, 0, SEEK_SET);
    
	*hash = *filesize;
    
	for (uint64_t tmp = 0, i = 0; i < 65536 / sizeof(tmp) && fread((char *) &tmp, sizeof(tmp), 1, handle); *hash += tmp, i++);
	fseek(handle, (*filesize - 65536) > 0 ? (*filesize - 65536) : 0, SEEK_SET);
	for (uint64_t tmp = 0, i = 0; i < 65536 / sizeof(tmp) && fread((char *) &tmp, sizeof(tmp), 1, handle); *hash += tmp, i++);
}

//////////////////////////////////////////////////////////////////////////////////////////////////
static int login(const char **token) {
	scoped_ptr(xmlrpc_value) result = NULL;
	scoped_ptr(xmlrpc_value) token_xmlval = NULL;
    
	xmlrpc_client_call2f(&env, client, STH_XMLRPC_URL, "LogIn", &result, "(ssss)", "", "", LOGIN_LANGCODE, LOGIN_USER_AGENT);
	if (env.fault_occurred) {
		log_err("login failed: %s (%d)", env.fault_string, env.fault_code);
		return env.fault_code;
	}
    
	xmlrpc_struct_find_value(&env, result, "token", &token_xmlval);
	xmlrpc_read_string(&env, token_xmlval, token);
    
	return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
/// convenience function the get a string value from a xmlrpc struct.
static const char *struct_get_char(xmlrpc_value *s, const char *key) {
    g_assert_nonnull(s);
    g_assert_nonnull(key);
    
	scoped_ptr(xmlrpc_value) xmlval = NULL;
	const char* str;
    
	xmlrpc_struct_find_value(&env, s, key, &xmlval);
	xmlrpc_read_string(&env, xmlval, &str);
    
	return str;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
static int search_get_results(const char *token, const char* hash_str, const char* filesize_str, const char *filename, xmlrpc_value **data) {
    g_assert_nonnull(token);
    g_assert_nonnull(hash_str);
    g_assert_nonnull(filesize_str);
    g_assert_nonnull(filename);
    
	scoped_ptr(xmlrpc_value) hash_query = NULL;
	scoped_ptr(xmlrpc_value) sublanguageid_xmlval = NULL;
	scoped_ptr(xmlrpc_value) hash_xmlval = NULL;
	scoped_ptr(xmlrpc_value) filesize_xmlval = NULL;
	scoped_ptr(xmlrpc_value) name_query = NULL;
	scoped_ptr(xmlrpc_value) filename_xmlval = NULL;
	scoped_ptr(xmlrpc_value) query_array = NULL;
	scoped_ptr(xmlrpc_value) limit_xmlval = NULL;
	scoped_ptr(xmlrpc_value) param_struct = NULL;
	scoped_ptr(xmlrpc_value) result = NULL;
    
	query_array = xmlrpc_array_new(&env);
    
	// create hash-based query
	if (!name_search_only) {
		hash_query = xmlrpc_struct_new(&env);
		sublanguageid_xmlval = xmlrpc_string_new(&env, lang);
		xmlrpc_struct_set_value(&env, hash_query, "sublanguageid", sublanguageid_xmlval);
		
		hash_xmlval = xmlrpc_string_new(&env, hash_str);
		xmlrpc_struct_set_value(&env, hash_query, "moviehash", hash_xmlval);
		
		filesize_xmlval = xmlrpc_string_new(&env, filesize_str);
		xmlrpc_struct_set_value(&env, hash_query, "moviebytesize", filesize_xmlval);
        
		xmlrpc_array_append_item(&env, query_array, hash_query);
	}
    
	// create full-text query
	if (!hash_search_only) {
		name_query = xmlrpc_struct_new(&env);
        
		sublanguageid_xmlval = xmlrpc_string_new(&env, lang);
		xmlrpc_struct_set_value(&env, name_query, "sublanguageid", sublanguageid_xmlval);
        
		filename_xmlval = xmlrpc_string_new(&env, filename);
		xmlrpc_struct_set_value(&env, name_query, "query", filename_xmlval);
        
		xmlrpc_array_append_item(&env, query_array, name_query);
	}
    
	// create parameter structure (currently only for "limit")
	param_struct = xmlrpc_struct_new(&env);
	limit_xmlval = xmlrpc_int_new(&env, limit);
	xmlrpc_struct_set_value(&env, param_struct, "limit", limit_xmlval);
    
	xmlrpc_client_call2f(&env, client, STH_XMLRPC_URL, "SearchSubtitles", &result, "(sAS)", token, query_array, param_struct);
	if (env.fault_occurred) {
		log_err("query failed: %s (%d)", env.fault_string, env.fault_code);
		return env.fault_code;
	}
    
	xmlrpc_struct_read_value(&env, result, "data", data);
	if (env.fault_occurred) {
		log_err("failed to get data: %s (%d)", env.fault_string, env.fault_code);
		return env.fault_code;
	}
    
	return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
static const char* find_imdb_from_nfo(const char *filepath) {
    g_assert_nonnull(filepath);
    
    bool parent_searched = false;
    
    scoped_ptr(GError) error = NULL;
    scoped_ptr(GMatchInfo) regex_match_info;
    scoped_ptr(GRegex) imdb_regex = g_regex_new("imdb\\.[^\\/]+\\/title\\/tt(\\d+)", G_REGEX_CASELESS, 0, NULL);
    scoped_ptr(GRegex) nfo_regex = g_regex_new(".*\\.nfo$|.*\\.txt$", G_REGEX_CASELESS, 0, NULL);
    scoped_ptr(GRegex) cd_regex = g_regex_new("disk\\d+$|cd\\d+$", G_REGEX_CASELESS, 0, NULL);
    
    // get directory
    scoped_ptr(char) dir_path = g_utf8_substring(filepath, 0, strrchr(filepath, '/') - filepath);
    scoped_ptr(GDir) dir = g_dir_open(dir_path, 0, &error);
    
    // search for nfo file
    while (true) {
        
        const char* filename = g_dir_read_name(dir);
        if (filename == NULL) {
            if (parent_searched == false && g_regex_match(cd_regex, dir_path, 0, NULL)) {
                parent_searched = true;
                
                // get parent directory
                dir_path[strrchr(dir_path, '/') - dir_path] = '\0';
                g_dir_close(dir);
                dir = g_dir_open(dir_path, 0, &error);
                filename = g_dir_read_name(dir);
            }
            else
                break;
        }
        
        if (g_regex_match(nfo_regex, filename, 0, NULL)) {
            scoped_ptr(gchar) nfo_contents = NULL;
            scoped_ptr(char) nfo_path = NULL;
            
            if (asprintf(&nfo_path, "%s/%s", dir_path, filename) == -1) {
                log_oom();
                return NULL;
            }

            // try to find imdb number with regexp in nfo file
            g_file_get_contents(nfo_path, &nfo_contents, NULL, &error);
            if (error != NULL) {
                log_err("Cannot read nfo file %s to find imdb id!\n"
                        "Error: %s", filepath, error->message);
                continue;
            }
            
            if (g_regex_match(imdb_regex, nfo_contents, 0, &regex_match_info)) {
                // imdb id found (first subcapture)
                return g_match_info_fetch(regex_match_info, 1);
            }
        }
    }
    
    return NULL;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
static int insert_moviehash(const char* token, const char* filepath, const char* hash_str, const char* filesize_str, const char* imdb_str) {
    g_assert_nonnull(token);
    g_assert_nonnull(hash_str);
    g_assert_nonnull(filesize_str);
    g_assert_nonnull(imdb_str);
    
    int err = 0;

    // get video technical info
    scoped_ptr(xmlrpc_value) movietimems_xmlval = NULL;
    scoped_ptr(xmlrpc_value) moviefps_xmlval = NULL;
    
    scoped_ptr(AVFormatContext) fmt_ctx = avformat_alloc_context();
    err = !avformat_open_input(&fmt_ctx, filepath, NULL, NULL) && avformat_find_stream_info(fmt_ctx, NULL);
    if (err == 0 && fmt_ctx->nb_streams > 0) {
        
        // get duration
        int64_t duration = (fmt_ctx->duration / AV_TIME_BASE) * 1000;
        
        scoped_ptr(char) movietimems_str = NULL;
        if (asprintf(&movietimems_str, "%lld", duration) == -1) {
            log_oom();
            return -1;
        }
        
        movietimems_xmlval = xmlrpc_string_new(&env, movietimems_str);
        
        // find the first video stream
        int i = 0;
        for(; i < fmt_ctx->nb_streams && fmt_ctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_VIDEO; ++i);
        
        // get fps
        if (i < fmt_ctx->nb_streams) {
            double fps = av_q2d(fmt_ctx->streams[i]->r_frame_rate);
            
            scoped_ptr(char) moviefps_str = NULL;
            if (asprintf(&moviefps_str, "%.3lf", fps) == -1) {
                log_oom();
                return -1;
            }
            
            moviefps_xmlval = xmlrpc_string_new(&env, moviefps_str);
        }
    }
    else
        log_err("Unable to get %s video info (AVFormatContext)!", filepath);

    // upload all information to opensubtitles.org
    scoped_ptr(xmlrpc_value) hash_xmlval = xmlrpc_string_new(&env, hash_str);
    scoped_ptr(xmlrpc_value) filesize_xmlval = xmlrpc_string_new(&env, filesize_str);
    scoped_ptr(xmlrpc_value) imdb_xmlval = xmlrpc_string_new(&env, imdb_str);
    
    scoped_ptr(xmlrpc_value) upload_query = xmlrpc_struct_new(&env);
    xmlrpc_struct_set_value(&env, upload_query, "moviehash", hash_xmlval);
    xmlrpc_struct_set_value(&env, upload_query, "moviebytesize", filesize_xmlval);
    xmlrpc_struct_set_value(&env, upload_query, "imdbid", imdb_xmlval);
    if (movietimems_xmlval != NULL)
        xmlrpc_struct_set_value(&env, upload_query, "movietimems", movietimems_xmlval);
    if (moviefps_xmlval != NULL)
        xmlrpc_struct_set_value(&env, upload_query, "moviefps", moviefps_xmlval);
    
    scoped_ptr(xmlrpc_value) query_array = xmlrpc_array_new(&env);
    xmlrpc_array_append_item(&env, query_array, upload_query);

    scoped_ptr(xmlrpc_value) result = NULL;
    xmlrpc_client_call2f(&env, client, STH_XMLRPC_URL, "InsertMovieHash", &result, "(sA)", token, query_array);

    scoped_ptr(xmlrpc_value) status = NULL;
    xmlrpc_struct_read_value(&env, result, "status", &status);
    
    scoped_cptr(char) status_str = NULL;
    xmlrpc_read_string(&env, status, &status_str);
    
    if (strcmp(status_str, "200 OK") == 0) {
        log_info("Info successfuly uploaded to opensubtitles.org");
    }
    else {
        log_err("Error while uploading info to opensubtitles.org");
    }
    
    return err;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
static void print_separator(int c, int digit_count) {
	for (int i = 0; i < c; i++) {
		if (i == digit_count + 1 ||
		    i == digit_count + 1 + 4 ||
		    i == digit_count + 1 + 4 + 6) {
			fputs(SEP_CROSS, stdout);
		}
		else {
			fputs(SEP_HORIZONTAL, stdout);
		}
	}
	putchar('\n');
}

//////////////////////////////////////////////////////////////////////////////////////////////////
static void print_table(struct sub_info *sub_infos, int n, int align_release_name) {
    g_assert_nonnull(sub_infos);
    
	// count number of digits
	int digit_count = 0;
	int n_tmp = n;
	while (n_tmp) {
		n_tmp /= 10;
		digit_count++;
	}
    
	// header
	putchar('\n');
	int c = printf("%-*c " SEP_VERTICAL " %c " SEP_VERTICAL " %s " SEP_VERTICAL " %-*s\n",
	               digit_count,
	               HEADER_ID,
	               HEADER_MATCHED_BY_HASH,
	               HEADER_LANG,
	               align_release_name,
	               HEADER_RELEASE_NAME);
    
	c -= 5;
    
	// separator
	print_separator(c, digit_count);
    
	// list
	for (int i = 0; i < n; i++) {
		printf("%-*i " SEP_VERTICAL " %c " SEP_VERTICAL " %s " SEP_VERTICAL " %-*s\n",
		       digit_count,
		       i + 1,
		       sub_infos[i].matched_by_hash ? '*' : ' ',
		       sub_infos[i].lang,
		       align_release_name,
		       sub_infos[i].release_name);
        
		printf("%-*s " SEP_VERTICAL "   " SEP_VERTICAL "     " SEP_VERTICAL " " SEP_UP_RIGHT "%s\n",
		       digit_count,
		       "",
		       sub_infos[i].filename);
        
		if (i != n - 1)
			print_separator(c, digit_count);
	}
	putchar('\n');
}

//////////////////////////////////////////////////////////////////////////////////////////////////
static int choose_from_results(xmlrpc_value *results, int *sub_id, const char **sub_filename) {
    g_assert_nonnull(results);
    g_assert_nonnull(sub_id);
    g_assert_nonnull(sub_filename);
    
	int err = 0;
    
	int n = xmlrpc_array_size(&env, results);
	if (env.fault_occurred) {
		log_err("failed to get array size: %s (%d)", env.fault_string, env.fault_code);
		return env.fault_code;
	}

    struct sub_info sub_infos[n];
    
	int sel = 0; // selected list item
    
	/* Make the values in the "Release / File Name" column
	 * at least as long as the header title itself. */
	int align_release_name = (int)strlen(HEADER_RELEASE_NAME);
    
	for (int i = 0; i < n; i++) {
		scoped_ptr(xmlrpc_value) oneresult = NULL;
		xmlrpc_array_read_item(&env, results, i, &oneresult);
        
		// dear OpenSubtitles.org, why are these IDs provided as strings?
		const char* sub_id_str = struct_get_char(oneresult, "IDSubtitleFile");
		const char* matched_by_str = struct_get_char(oneresult, "MatchedBy");
        
		sub_infos[i].id = (int)strtol(sub_id_str, NULL, 10);
		sub_infos[i].matched_by_hash = strcmp(matched_by_str, "moviehash") == 0;
		sub_infos[i].lang = struct_get_char(oneresult, "SubLanguageID");
		sub_infos[i].release_name = struct_get_char(oneresult, "MovieReleaseName");
		sub_infos[i].filename = struct_get_char(oneresult, "SubFileName");
        
		// select first hash match if one exists
		if (sub_infos[i].matched_by_hash && sel == 0)
			sel = i + 1;
        
		int s = (int)strlen(sub_infos[i].release_name);
		if (s > align_release_name)
			align_release_name = s;
        
		s = (int)strlen(sub_infos[i].filename);
		if (s > align_release_name)
			align_release_name = s;
	}
    
	if (never_ask && sel == 0)
		sel = 1;
    
	if (sel == 0 || always_ask) {
		print_table(sub_infos, n, align_release_name);
        
		scoped_ptr(char) line = NULL;
		size_t len = 0;
		char *endptr = NULL;
		do {
			printf("Choose subtitle [1..%i]: ", n);
			if (getline(&line, &len, stdin) == -1) {
				err = EIO;
				goto finish;
			}
			
			sel = (int)strtol(line, &endptr, 10);
		} while (*endptr != '\n' || sel < 1 || sel > n);
	}
	else if (!quiet) {
		print_table(sub_infos, n, align_release_name);
	}
    
	*sub_id = sub_infos[sel - 1].id;
	*sub_filename = strdup(sub_infos[sel - 1].filename);
    
	if (!*sub_filename) {
		err = log_oom();
		goto finish;
	}
    
finish:
	// __attribute__(cleanup) can't be used in structs, let alone arrays
	for (int i = 0; i < n; i++)
        cleanup_sub_info(&sub_infos[i]);
    
	return err;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
static int sub_download(const char *token, int sub_id, const char *file_path) {
    g_assert_nonnull(token);
    g_assert_nonnull(file_path);
    g_assert_true(sub_id > 0);
    
    int err = 0;
    
	scoped_ptr(xmlrpc_value) sub_id_xmlval = NULL;
	scoped_ptr(xmlrpc_value) query_array = NULL;
	scoped_ptr(xmlrpc_value) result = NULL;;
    
	scoped_ptr(xmlrpc_value) data = NULL;       // result -> data
	scoped_ptr(xmlrpc_value) data_0 = NULL;     // result -> data[0]
	scoped_ptr(xmlrpc_value) data_0_sub = NULL; // result -> data[0][data]
    
	scoped_cptr(char) sub_base64 = NULL;	  // the subtitle, gzipped and base64 encoded
    
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
    
	scoped_ptr(FILE) f = NULL;
    
	// check if file already exists
	if (access(file_path, F_OK) == 0) {
		if (force_overwrite) {
			log_info("file already exists, overwriting.");
		} else {
			log_err("file already exists, aborting. Use -f to force an overwrite.");
			return EEXIST;
		}
	}
    
	// download
	sub_id_xmlval = xmlrpc_int_new(&env, sub_id);
    
	query_array = xmlrpc_array_new(&env);
	xmlrpc_array_append_item(&env, query_array, sub_id_xmlval);
    
	xmlrpc_client_call2f(&env, client, STH_XMLRPC_URL, "DownloadSubtitles", &result, "(sA)", token, query_array);
	if (env.fault_occurred) {
		log_err("query failed: %s (%d)", env.fault_string, env.fault_code);
		return env.fault_code;
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
		return errno;
	}
    
	// 16+MAX_WBITS is needed for gzip support
	z_ret = inflateInit2(&z_strm, 16 + MAX_WBITS);
	if (z_ret != Z_OK) {
		log_err("failed to init zlib (%i)", z_ret);
		return z_ret;
	}
    
	int b64_state = 0;
	unsigned int b64_save = 0;
	unsigned int b64_offset = 0;
	do {
		// write decoded data to z_in
		z_strm.avail_in = (unsigned int)g_base64_decode_step(&sub_base64[b64_offset], ZLIB_CHUNK, z_in, &b64_state, &b64_save);
		b64_offset += z_strm.avail_in * 4 / 3; //  base64 encodes 3 bytes in 4 chars
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
                    err = z_ret;
                    log_err("zlib error: %s (%d)", z_strm.msg, z_ret);
                    goto finish;
			}
			// write decompressed data from z_out to file
			unsigned int have = ZLIB_CHUNK - z_strm.avail_out;
			fwrite(z_out, 1, have, f);
		} while (z_strm.avail_out == 0);
	} while (z_ret != Z_STREAM_END);
    
finish:
	inflateEnd(&z_strm);
    
	return err;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
static void show_subberthehut_usage() {
	puts("Usage: subberthehut [options] <file>...\n\n"
         
	     "OpenSubtitles.org downloader.\n\n"
         
	     "subberthehut can do a hash-based and a name-based search.\n"
	     "On a hash-based search, subberthehut will generate a hash from the specified\n"
	     "video file and use this to search for appropriate subtitles.\n"
	     "Any results from this hash-based search should be compatible\n"
	     "with the video file. Therefore subberthehut will, by default, automatically\n"
	     "download the first subtitle from these search results.\n"
	     "In case the hash-based search returns no results, subberthehut will also\n"
	     "do a name-based search, meaning the OpenSubtitles.org database\n"
	     "will be searched with the filename of the specified file. The results\n"
	     "from this search are not guaranteed to be compatible with the video\n"
	     "file. Therefore subberthehut will, by default, ask the user which subtitle to\n"
	     "download.\n"
	     "Results from the hash-based search are marked with an asterisk (*)\n"
	     "in the 'H' column.\n\n"
         
	     "Options:\n"
	     " -h, --help              Show this help and exit.\n"
	     "\n"
	     " -v, --version           Show version information and exit.\n"
	     "\n"
	     " -l, --lang <languages>  Comma-separated list of languages to search for,\n"
	     "                         e.g. 'eng,ger'. Use 'all' to search for all\n"
	     "                         languages. Default is 'eng'. Use --list-languages\n"
	     "                         to list all available languages.\n"
	     "\n"
	     " -L, --list-languages    List all available languages and exit.\n"
	     "\n"
	     " -a, --always-ask        Always ask which subtitle to download, even\n"
	     "                         when there are hash-based results.\n"
	     "\n"
	     " -n, --never-ask         Never ask which subtitle to download, even\n"
	     "                         when there are only name-based results.\n"
	     "                         When this option is specified, the first\n"
	     "                         search result will be downloaded.\n"
	     "\n"
	     " -f, --force             Overwrite output file if it already exists.\n"
	     "\n"
	     " -o, --hash-search-only  Only do a hash-based search.\n"
	     "\n"
	     " -O, --name-search-only  Only do a name-based search. This is useful in\n"
	     "                         case of false positives from the hash-based search.\n"
	     "\n"
	     " -s, --same-name         Download the subtitle to the same filename as the\n"
	     "                         original file, only replacing the file extension.\n"
	     "\n"
         " -u  --no-upload-info    Disable uploading info to opensubtitles.org. While\n"
         "                         searching for subtitles, program will try to find\n"
         "                         imdb number of movie and upload it with hash, fps,\n"
         "                         duration and filesize.\n"
         "\n"
	     " -t, --limit <number>    Limits the number of returned results. The default is 10.\n"
	     "\n"
	     " -e, --no-exit-on-fail   By default, subberthehut will exit immediately if\n"
	     "                         multiple files are passed and it fails to download\n"
	     "                         a subtitle for one them. When this option is passed,\n"
	     "                         subberthehut will process the next file(s) regardless.\n"
	     "\n"
	     " -q, --quiet             Don't print the table if the user doesn't have to be\n"
	     "                         asked which subtitle to download. Pass this option twice\n"
	     "                         to suppress anything but warnings and error messages.\n");
}

//////////////////////////////////////////////////////////////////////////////////////////////////
static void show_subberthehut_version() {
	puts("subberthehut " VERSION "\n"
	     "https://github.com/mus65/subberthehut/");
}

//////////////////////////////////////////////////////////////////////////////////////////////////
static const char *get_sub_path(const char *filepath, const char *sub_filename) {
    g_assert_nonnull(filepath);
    g_assert_nonnull(sub_filename);
    
	char *sub_filepath;
    
	if (same_name) {
		const char *sub_ext = strrchr(sub_filename, '.');
		if (!sub_ext) {
			log_err("warning: subtitle filename from the OpenSubtitles.org "
			        "database has no file extension, assuming .srt.");
			sub_ext = ".srt";
		}
		const char *lastdot = strrchr(filepath, '.');
		size_t index;
		if (!lastdot)
			index = strlen(filepath) - 1;
		else
			index = (lastdot - filepath);
        
		sub_filepath = malloc(index + 1 + strlen(sub_ext) + 1);
		if (!sub_filepath)
			return NULL;
        
		strncpy(sub_filepath, filepath, index);
		sub_filepath[index] = '\0';
		strcat(sub_filepath, sub_ext);
	} else {
		const char *lastslash = strrchr(filepath, '/');
		if (!lastslash) {
			sub_filepath = strdup(sub_filename);
			if (!sub_filepath)
				return NULL;
		} else {
			long index = (lastslash - filepath);
			sub_filepath = malloc(index + 1 + strlen(sub_filename) + 1);
			if (!sub_filepath)
				return NULL;
            
			strncpy(sub_filepath, filepath, index + 1);
			sub_filepath[index + 1] = '\0';
			strcat(sub_filepath, sub_filename);
		}
	}
	return sub_filepath;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
static int process_file(const char *token, const char *filepath) {
    g_assert_nonnull(filepath);
    g_assert_nonnull(token);
    
	scoped_ptr(FILE) f = NULL;
    
	scoped_ptr(xmlrpc_value) results = NULL;
    
	scoped_cptr(char) sub_filename = NULL;
	scoped_cptr(char) sub_filepath = NULL;
    
    scoped_ptr(char) filesize_str = NULL;
    scoped_ptr(char) hash_str = NULL;
    scoped_cptr(char) imdb_str = NULL;
    
	int err = 0;
    
	// get hash/filesize
	if (!name_search_only) {
		f = fopen(filepath, "r");
		if (!f) {
			log_err("failed to open %s: %m", filepath);
			return errno;
		}
        
        uint64_t hash = 0;
        uint64_t filesize = 0;
		get_hash_and_filesize(f, &hash, &filesize);
        
        if (asprintf(&hash_str, "%016" PRIx64, hash) == -1 ||
            asprintf(&filesize_str, "%" PRIu64, filesize) == -1) {
            log_oom();
            return -1;
        }
	}
    
	const char *filename = strrchr(filepath, '/');
	if (filename)
		filename++; // skip '/'
	else
		filename = filepath;
    
	log_info("searching for %s...", filename);
    
	err = search_get_results(token, hash_str, filesize_str, filename, &results);
	if (err != 0)
		return err;
    
	// for some reason [data] is of type XMLRPC_TYPE_BOOL if the search returns no hits!?
	if (xmlrpc_value_type(results) != XMLRPC_TYPE_ARRAY) {
		log_err("no results.");
		return 1;
	}
	// let user choose the subtitle to download
	int sub_id = 0;
	err = choose_from_results(results, &sub_id, &sub_filename);
	if (err != 0)
		return err;
    
    // upload info to opensubtitles.org
    if (upload_info) {
        imdb_str = find_imdb_from_nfo(filepath);
        if (imdb_str != NULL)
            insert_moviehash(token, filepath, hash_str, filesize_str, imdb_str);
    }
    
	sub_filepath = get_sub_path(filepath, sub_filename);
	if (!sub_filepath)
		return log_oom();
    
    // download subtitle
	log_info("Downloading to %s ...", sub_filepath);
	err = sub_download(token, sub_id, sub_filepath);
    
	return err;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
static int list_sub_languages() {
	scoped_ptr(xmlrpc_value) result = NULL;
	scoped_ptr(xmlrpc_value) languages = NULL;
    
	xmlrpc_client_call2f(&env, client, STH_XMLRPC_URL, "GetSubLanguages", &result, "()");
	if (env.fault_occurred) {
		log_err("failed to download languages: %s (%d)", env.fault_string, env.fault_code);
		return env.fault_code;
	}
    
	xmlrpc_struct_read_value(&env, result, "data", &languages);
	if (env.fault_occurred) {
		log_err("failed to get data: %s (%d)", env.fault_string, env.fault_code);
		return env.fault_code;
	}
    
	int n = xmlrpc_array_size(&env, languages);
	if (env.fault_occurred) {
		log_err("failed to get array size: %s (%d)", env.fault_string, env.fault_code);
		return env.fault_code;
	}
    
	for (int i = 0; i < n; i++) {
		scoped_ptr(xmlrpc_value) language = NULL;
		xmlrpc_array_read_item(&env, languages, i, &language);
        
		const char *lang_id = struct_get_char(language, "SubLanguageID");
		const char *lang_name = struct_get_char(language, "LanguageName");
        
		printf("%s - %s\n", lang_id, lang_name);
	}
    
	return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
int main(int argc, char *argv[]) {
	scoped_cptr(char) token = NULL;
    
	int ret = EXIT_SUCCESS;
    
	// parse options
	const struct option opts[] = {
		{"help", no_argument, NULL, 'h'},
		{"lang", required_argument, NULL, 'l'},
		{"list-languages", no_argument, NULL, 'L'},
		{"always-ask", no_argument, NULL, 'a'},
		{"never-ask", no_argument, NULL, 'n'},
		{"force", no_argument, NULL, 'f'},
		{"hash-search-only", no_argument, NULL, 'o'},
		{"name-search-only", no_argument, NULL, 'O'},
		{"same-name", no_argument, NULL, 's'},
        {"no-upload-info", no_argument, NULL, 'u'},
		{"limit", required_argument, NULL, 't'},
		{"no-exit-on-fail", no_argument, NULL, 'e'},
		{"quiet", no_argument, NULL, 'q'},
		{"version", no_argument, NULL, 'v'},
		{0, 0, 0, 0}
	};
    
	int c;
	while ((c = getopt_long(argc, argv, "hl:LanfoOst:eqv", opts, NULL)) != -1) {
		switch (c) {
            case 'h':
                show_subberthehut_usage();
                return EXIT_SUCCESS;
                
            case 'l':
                lang = optarg;
                break;
                
            case 'L':
                list_languages = true;
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
                name_search_only = false;
                break;
                
            case 'O':
                name_search_only = true;
                hash_search_only = false;
                break;
                
            case 's':
                same_name = true;
                break;
                
            case 'r':
                break;
                
            case 'i':
                break;
                
            case 't':
            {
                char *endptr = NULL;
                limit = (int)strtol(optarg, &endptr, 10);
                
                if (*endptr != '\0' || limit < 1) {
                    log_err("invalid limit: %s", optarg);
                    return EXIT_FAILURE;
                }
                break;
            }
                
            case 'e':
                exit_on_fail = false;
                break;
                
            case 'q':
                quiet++;
                break;
                
            case 'v':
                show_subberthehut_version();
                return EXIT_SUCCESS;
                
            default:
                return EXIT_FAILURE;
		}
	}
    
	// check if user has specified at least one file (except for listing languages)
	if (argc - optind < 1 && !list_languages) {
		show_subberthehut_usage();
		return EXIT_FAILURE;
	}
    
    // regexp init
    scoped_ptr(GRegex) video_regexp = g_regex_new(".*\\.3g2$|.*\\.3gp$|.*\\.3gp2$|.*\\.3gpp$|.*\\.60d$|.*\\.ajp$|.*\\.asf$|.*\\.asx$|.*\\.avchd$|.*\\.avi$|.*\\.bik$|.*\\.bix$|.*\\.box$|.*\\.cam$|.*\\.dat$|.*\\.divx$|.*\\.dmf$|.*\\.dv$|.*\\.dvr-ms$|.*\\.evo$|.*\\.flc$|.*\\.fli$|.*\\.flic$|.*\\.flv$|.*\\.flx$|.*\\.gvi$|.*\\.gvp$|.*\\.h264$|.*\\.m1v$|.*\\.m2p$|.*\\.m2ts$|.*\\.m2v$|.*\\.m4e$|.*\\.m4v$|.*\\.mjp$|.*\\.mjpeg$|.*\\.mjpg$|.*\\.mkv$|.*\\.moov$|.*\\.mov$|.*\\.movhd$|.*\\.movie$|.*\\.movx$|.*\\.mp4$|.*\\.mpe$|.*\\.mpeg$|.*\\.mpg$|.*\\.mpv$|.*\\.mpv2$|.*\\.mxf$|.*\\.nsv$|.*\\.nut$|.*\\.ogg$|.*\\.ogm$|.*\\.omf$|.*\\.ps$|.*\\.qt$|.*\\.ram$|.*\\.rm$|.*\\.rmvb$|.*\\.swf$|.*\\.ts$|.*\\.vfw$|.*\\.vid$|.*\\.video$|.*\\.viv$|.*\\.vivo$|.*\\.vob$|.*\\.vro$|.*\\.wm$|.*\\.wmv$|.*\\.wmx$|.*\\.wrap$|.*\\.wvx$|.*\\.wx$|.*\\.x264$|.*\\.xvid$", G_REGEX_CASELESS, 0, NULL);

    // ffmpeg init
    av_register_all();
    
	// xmlrpc init
	xmlrpc_env_init(&env);
	xmlrpc_client_setup_global_const(&env);
	// make sure the library doesn't complain about too much data
	xmlrpc_limit_set(XMLRPC_XML_SIZE_LIMIT_ID, STH_XMLRPC_SIZE_LIMIT);
    
	xmlrpc_client_create(&env, XMLRPC_CLIENT_NO_FLAGS, "subberthehut", VERSION, NULL, 0, &client);
	if (env.fault_occurred) {
		log_err("failed to init xmlrpc client: %s (%d)", env.fault_string, env.fault_code);
		ret = env.fault_code;
		goto finish;
	}
	// login
	ret = login(&token);
	if (ret != 0) {
        log_err("failed to login to opensubtitles.org");
		goto finish;
    }
    
	// only list the languages and exit
	if (list_languages) {
		ret = list_sub_languages();
		goto finish;
	}
    
	// process files
	for (int i = optind; i < argc; i++) {
		char *filepath = argv[i];
        
        // check extension of file
        if (g_regex_match(video_regexp, filepath, 0, NULL)) {
            ret = process_file(token, filepath);
        }
        else {
            log_err("%s is not a video file (invalid extension)", filepath);
            ret = 1;
        }
        
        if (ret != 0) {
            if (exit_on_fail)
                goto finish;
            else
                log_info("Cannot download subtitle for %s", filepath);
        }
	}
    
finish:
	xmlrpc_env_clean(&env);
	xmlrpc_client_destroy(client);
	xmlrpc_client_teardown_global_const();
    
	return ret;
}