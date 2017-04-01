#include <ctype.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/stat.h>

#include "lcmgen.h"

#define INDENT(n) (4*(n))

#define emit_start(n, ...) do { fprintf(f, "%*s", INDENT(n), ""); fprintf(f, __VA_ARGS__); } while (0)
#define emit_continue(...) do { fprintf(f, __VA_ARGS__); } while (0)
#define emit_end(...) do { fprintf(f, __VA_ARGS__); fprintf(f, "\n"); } while (0)
#define emit(n, ...) do { fprintf(f, "%*s", INDENT(n), ""); fprintf(f, __VA_ARGS__); fprintf(f, "\n"); } while (0)

void setup_rust_options(getopt_t *gopt)
{
    getopt_add_string (gopt, 0, "rust-path",    ".",      "Location for .rs files");
}

static char *dots_to_slashes(const char *s)
{
    char *p = strdup(s);
    for (char *t=p; *t!=0; t++)
        if (*t == '.')
            *t = G_DIR_SEPARATOR;
    return p;
}

static char *
dots_to_double_colons(const char *s)
{
    // allocate the maximum possible amount of space needed
    char* p = (char*) calloc(1, 2 * strlen(s) + 1);
    char* q = p;

    for (const char *t=s; *t!=0; t++) {
        if (*t == '.') {
            *q = ':';
            q++;
            *q = ':';
        } else
            *q = *t;
        q++;
    }

    return p;
}

static char* make_rust_file_path(const char* prefix, const lcm_struct_t* lcm_struct) {
    // allocate space for modfile name
    char* package_name = lcm_struct->structname->package;
    char* result = calloc(strlen(prefix) + 1 + strlen(package_name) + 1, sizeof(char));
    if (result == NULL) {
        return NULL;
    }
    result[0] = '\0';

    // Build modfile path
    strcat(result, prefix);
    strcat(result, "/");
    strcat(result, package_name);
    for (char* c = result + strlen(prefix); *c != 0; ++c) {
        if (*c == '.') {
            *c = '/';
        }
    }
    return result;
}

static char* make_rust_mod_file_name(const char* prefix, const lcm_struct_t* lcm_struct) {
    static const char* modfile_suffix = "/mod.rs";
    char* path = make_rust_file_path(prefix, lcm_struct);
    if (path == NULL) {
        return NULL;
    }
    char* result = calloc(strlen(path) + strlen(modfile_suffix) + 1, sizeof(char));
    if (result ==NULL) {
        free(path);
        return NULL;
    }
    strcat(result, path);
    strcat(result, modfile_suffix);

    free(path);
    return result;
}

static char* make_rust_file_name(const char* prefix, const lcm_struct_t* lcm_struct) {
    static const char* rust_suffix = ".rs";
    char* path = make_rust_file_path(prefix, lcm_struct);
    if (path == NULL) {
        return NULL;
    }
    char* result = calloc(strlen(path) + 1 + // path + '/'
                          strlen(lcm_struct->structname->shortname) +
                          strlen(rust_suffix) + 1, // suffix + \0
                          sizeof(char));
    if (result == NULL) {
        free(path);
        return NULL;
    }
    strcat(result, path);
    strcat(result, "/");
    strcat(result, lcm_struct->structname->shortname);
    strcat(result, rust_suffix);
    printf("The path is %s\n", result);

    free(path);
    return result;
}

static char * make_rust_type_name(const lcm_typename_t *typename) {
    char* result = strdup(typename->shortname);

    // Convert to camel case
    char* result_char = result;
    int capitalize_next_char = 1;
    for (const char* c = typename->shortname; *c != 0; ++c) {
        if (*c == '_') {
            capitalize_next_char = 1;
        } else {
            if (capitalize_next_char) {
                capitalize_next_char = 0;
                *result_char = toupper(*c);
            } else {
                *result_char = tolower(*c);
            }
            ++result_char;
        }
    }
    *result_char = 0;

    // Special case:
    // For type names following the C convention of *_t, remove the _t suffix
    // (or rather, the trailing 'T', since we've already converted to camel case)
    if (result_char - result > 2 && *(result_char - 1) == 'T')
        *(result_char - 1) = 0;

    return result;
}

static char * make_rustdoc_comment(const char *comment) {
    int lines = 1;
    for(const char *c = comment; *c != 0; c++){
        if (*c == '\n') lines++;
    }

    char *result = calloc(4*lines + strlen(comment) + 1, sizeof(char));
    if (result == NULL) {
        return NULL;
    }

    int result_index = 0;
    result[result_index++] = '/';
    result[result_index++] = '/';
    result[result_index++] = '/';
    result[result_index++] = ' ';
    for(const char *c = comment; *c != 0; c++) {
        result[result_index++] = *c;
        if (*c == '\n') {
            result[result_index++] = '/';
            result[result_index++] = '/';
            result[result_index++] = '/';
            result[result_index++] = ' ';
        }
    }
    result[result_index] = 0;

    return result;
}

static const char * dim_size_prefix(const char *dim_size) {
    char *eptr = NULL;
    long asdf = strtol(dim_size, &eptr, 0);
    (void) asdf;  // suppress compiler warnings
    if(*eptr == '\0')
        return "";
    else
        return "this->";
}

static int is_dim_size_fixed(const char* dim_size) {
    char *eptr = NULL;
    long asdf = strtol(dim_size, &eptr, 0);
    (void) asdf;  // suppress compiler warnings
    return (*eptr == '\0');
}

static char *map_lcm_primative(const char *typename)
{
    if (!strcmp(typename, "boolean"))
        return strdup("bool");

    if (!strcmp(typename, "string"))
        return strdup("String");

    if (!strcmp(typename, "byte"))
        return strdup("u8");

    if (!strcmp(typename, "int8_t"))
        return strdup("i8");

    if (!strcmp(typename, "int16_t"))
        return strdup("i16");

    if (!strcmp(typename, "int32_t"))
        return strdup("i32");

    if (!strcmp(typename, "int64_t"))
        return strdup("i64");

    if (!strcmp(typename, "uint8_t"))
        return strdup("u8");

    if (!strcmp(typename, "uint16_t"))
        return strdup("u16");

    if (!strcmp(typename, "uint32_t"))
        return strdup("u32");

    if (!strcmp(typename, "uint64_t"))
        return strdup("u64");

    if (!strcmp(typename, "float"))
        return strdup("f32");

    if (!strcmp(typename, "double"))
        return strdup("f64");

    return NULL;
}

static char *map_type_name(const lcm_typename_t *typename)
{
    char *t = map_lcm_primative(typename->shortname);
    if (t) {
        return t;
    }

    return make_rust_type_name(typename);
}

static void make_dirs_for_file(const char *path)
{
#ifdef WIN32
    char *dirname = g_path_get_dirname(path);
    g_mkdir_with_parents(dirname, 0755);
    g_free(dirname);
#else
    int len = strlen(path);
    for (int i = 0; i < len; i++) {
        if (path[i]=='/') {
            char *dirpath = (char *) malloc(i+1);
            strncpy(dirpath, path, i);
            dirpath[i]=0;

            mkdir(dirpath, 0755);
            free(dirpath);

            i++; // skip the '/'
        }
    }
#endif
}

static void emit_header_start(lcmgen_t *lcmgen, FILE *f)
{
    emit(0, "// GENERATED CODE - DO NOT EDIT");
    emit(0, "");
    emit(0, "use lcm::Message;");
    emit(0, "use std::io::{Result, Read, Write};");
}

static void emit_struct_def(lcmgen_t *lcmgen, FILE *f, lcm_struct_t *lcm_struct)
{
    char *struct_name = make_rust_type_name(lcm_struct->structname);

    // Include non-primitive types - assuming they're in the same parent module as this
    for (unsigned int mind = 0; mind < g_ptr_array_size(lcm_struct->members); mind++) {
        lcm_member_t *lm = (lcm_member_t *)g_ptr_array_index(lcm_struct->members, mind);
        if (!lcm_is_primitive_type(lm->type->lctypename) && 
            strcmp(lm->type->lctypename, lcm_struct->structname->lctypename)) {
            char *mapped_tn = map_type_name(lm->type);
            char *other_pn = dots_to_double_colons(lm->type->package);
            emit(0, "use super::%s::%s;", other_pn, mapped_tn);
            free(other_pn);
            free(mapped_tn);
        }
    }
    emit(0, "");

    // The struct
    if (lcm_struct->comment != NULL) {
        char *comment = make_rustdoc_comment(lcm_struct->comment);
        emit(0, "%s", comment);
        free(comment);
    }
    emit(0, "#[derive(Debug, Default)]");
    emit(0, "pub struct %s {", struct_name);

    // Iterate over members of this struct
    // Arrays are represented by a Vec (for dynamically sized dimensions)
    // or by a GenericArray (for constant sized dimensions)
    for (unsigned int mind = 0; mind < g_ptr_array_size(lcm_struct->members); ++mind) {
        lcm_member_t* member = (lcm_member_t*) g_ptr_array_index(lcm_struct->members, mind);

        int ndim = g_ptr_array_size(member->dimensions);
        if (member->comment != NULL) {
            char *comment = make_rustdoc_comment(member->comment);
            emit(1, "%s", comment);
            free(comment);
        }
        emit_start(1, "pub %s: ", member->membername);

        // Iterate forwards and open the array declaration
        for (unsigned int d = 0; d < ndim; ++d) {
            lcm_dimension_t *dimension = (lcm_dimension_t*) g_ptr_array_index(member->dimensions, d);
            switch (dimension->mode) {
            case LCM_CONST: {
                emit_continue("[");
                break;
            }
            case LCM_VAR: {
                emit_continue("Vec<");
                break;
            }
            }
        }

        {
            char *mapped_typename = map_type_name(member->type);
            emit_continue("%s", mapped_typename);
            free(mapped_typename);
        }

        // Iterate backwards and close the array declaration
        for (unsigned int d = ndim; d-- > 0; ) {
            lcm_dimension_t *dimension = (lcm_dimension_t*) g_ptr_array_index(member->dimensions, d);
            switch (dimension->mode) {
            case LCM_CONST: {
                emit_continue("; %s]", dimension->size);
                break;
            }
            case LCM_VAR: {
                emit_continue(">");
                break;
            }
            }
        }
        emit_end(",");
    }
    emit(0, "}");
    emit(0, "");

    free(struct_name);
}

static void emit_impl_struct(lcmgen_t *lcmgen, FILE *f, lcm_struct_t *lcm_struct)
{
    // Constants
    if (g_ptr_array_size(lcm_struct->constants) > 0) {
        char *type_name = make_rust_type_name(lcm_struct->structname);
        emit(0, "impl %s {", type_name);
        emit(0, "");

        for (unsigned int i = 0; i < g_ptr_array_size(lcm_struct->constants); i++) {
            lcm_constant_t *lc = (lcm_constant_t *) g_ptr_array_index(lcm_struct->constants, i);
            assert(lcm_is_legal_const_type(lc->lctypename));

            if (lc->comment != NULL) {
                char *comment = make_rustdoc_comment(lc->comment);
                emit(1, "%s", comment);
                free(comment);
            }

            char *mapped_typename = map_lcm_primative(lc->lctypename);
            emit(1, "#[allow(non_snake_case)]");
            emit(1, "pub fn %s() -> %s {", lc->membername, mapped_typename);
            emit(2, "%s", lc->val_str);
            emit(1, "}");
            free(mapped_typename);

            emit(0, "");
        }

        emit(0, "}");
        emit(0, "");

        free(type_name);
    }
}

static void emit_impl_message_hash(FILE *f, lcm_struct_t *lcm_struct) {
    emit(1,     "fn hash() -> u64 {");
    emit(2,         "let hash = 0x%016"PRIx64";", lcm_struct->hash);
    emit(2,         "(hash << 1) + ((hash >> 63) & 1)");
    emit(1,     "}");
    emit(0, "");
}

static void emit_impl_message_encode(FILE *f, lcm_struct_t *lcm_struct) {
    unsigned int n_members = g_ptr_array_size(lcm_struct->members);
    emit(1, "fn encode(&self, %s: &mut Write) -> Result<()> {", n_members ? "mut buffer" : "_");
    for (unsigned int mind = 0; mind < n_members; mind++) {
        lcm_member_t *member = (lcm_member_t *) g_ptr_array_index(lcm_struct->members, mind);
        int ndim = g_ptr_array_size(member->dimensions);

        emit(2, "let item = &self.%s;", member->membername);
        for (unsigned int d = 0; d != ndim; ++d) {
            lcm_dimension_t *dimension = (lcm_dimension_t *) g_ptr_array_index(member->dimensions, d);
            emit(2+d, "for item in item.iter() {");
        }
        emit(2+ndim, "item.encode(&mut buffer)?;");
        for (unsigned int d = ndim; d-- != 0; ) {
            emit(2+d, "}");
        }
    }
    emit(2, "Ok(())");
    emit(1, "}");
    emit(0, "");
}

static void emit_impl_message_decode_recursive(FILE *f, lcm_member_t *member, unsigned int dim) {
    if (dim == g_ptr_array_size(member->dimensions)) {
        emit_end("");
        emit_start(3+dim, "Message::decode(&mut buffer)");

        if (dim == 0 || ((lcm_dimension_t*)g_ptr_array_index(member->dimensions, dim-1))->mode == LCM_CONST) {
            emit_continue("?");
        }

        return;
    }
   
    lcm_dimension_t *dimension = (lcm_dimension_t*) g_ptr_array_index(member->dimensions, dim);
    switch (dimension->mode) {
    case LCM_CONST: {
        emit_continue("[");
        int size;
        sscanf(dimension->size, "%d", &size);
        for (int i = 0; i != size; ++i) {
            emit_impl_message_decode_recursive(f, member, dim+1);
            emit_continue(",");
        }
        emit_end("");
        emit_start(2+dim, "]");
        break;
    }
    case LCM_VAR: {
        emit_end("");
        emit_start(3+dim, "(0..%s).map(|_| {", dimension->size);
        emit_impl_message_decode_recursive(f, member, dim+1);
        emit_end("");
        emit_start(3+dim, "}).collect::<Result<_>>()");
        
        if (dim == 0 || ((lcm_dimension_t*)g_ptr_array_index(member->dimensions, dim-1))->mode == LCM_CONST) {
            emit_continue("?");
        }
        break;
    }
    }
}

static void emit_impl_message_decode(FILE *f, lcm_struct_t *lcm_struct) {
    char *type_name = make_rust_type_name(lcm_struct->structname);
    unsigned int n_members = g_ptr_array_size(lcm_struct->members);

    emit(1, "fn decode(%s: &mut Read) -> Result<Self> {", n_members ? "mut buffer" : "_");
    for (unsigned int mind = 0; mind < n_members; mind++) {
        lcm_member_t *member = (lcm_member_t *) g_ptr_array_index(lcm_struct->members, mind);
        int ndim = g_ptr_array_size(member->dimensions);

        emit_start(2, "let %s = ", member->membername);
        emit_impl_message_decode_recursive(f, member, 0);
        emit_end(";");
        emit(0, "");
    }

    emit(2, "Ok(%s {", type_name);
    for (unsigned int mind = 0; mind < g_ptr_array_size(lcm_struct->members); mind++) {
        lcm_member_t *member = (lcm_member_t *) g_ptr_array_index(lcm_struct->members, mind);

        emit(3, "%s: %s,", member->membername, member->membername);
    }
    emit(2, "})");
    emit(1, "}");
    emit(0, "");

    free(type_name);
}

static void emit_impl_message_size(FILE *f, lcm_struct_t *lcm_struct) {
    emit(1, "fn size(&self) -> usize {");
    emit(2, "0");
    for (unsigned int mind = 0; mind < g_ptr_array_size(lcm_struct->members); mind++) {
        lcm_member_t *member = (lcm_member_t *) g_ptr_array_index(lcm_struct->members, mind);
        int ndim = g_ptr_array_size(member->dimensions);
        
        emit_start(2, "+ self.%s", member->membername);
        if (ndim > 0) {
            for (unsigned int d = 0; d != ndim; ++d) {
                if (d == 0) {
                    emit_continue(".iter()");
                } else {
                    emit_continue(".flat_map(IntoIterator::into_iter)");
                }
            }
            emit_end(".map(Message::size).sum::<usize>()");
        } else {
            emit_end(".size()");
        }
    }
    emit(1, "}");
}

static void emit_impl_message(lcmgen_t *lcmgen, FILE *f, lcm_struct_t *lcm_struct)
{
    char *type_name = make_rust_type_name(lcm_struct->structname);

    emit(0, "impl Message for %s {", type_name);

    emit_impl_message_hash(f, lcm_struct);
    emit_impl_message_encode(f, lcm_struct);
    emit_impl_message_decode(f, lcm_struct);
    emit_impl_message_size(f, lcm_struct);

    emit(0, "}");
    emit(0, "");

    free(type_name);
}

int emit_rust(lcmgen_t *lcmgen)
{
    // compute the target filename
    char *rust_path = getopt_get_string(lcmgen->gopt, "rust-path");
    printf("rust-path: %s\n", rust_path);

    // Remove mod.rs for each module
    for (unsigned int i = 0; i < g_ptr_array_size(lcmgen->structs); ++i) {
        lcm_struct_t *lcm_struct = (lcm_struct_t*) g_ptr_array_index(lcmgen->structs, i);
        char* modfile_name = make_rust_mod_file_name(rust_path, lcm_struct);
        if (remove(modfile_name) == 0) {
            printf("Removed file: %s\n", modfile_name);
        }
        free(modfile_name);
    }

    // Emit headers for all modules
    for (unsigned int i = 0; i < g_ptr_array_size(lcmgen->structs); ++i) {
        lcm_struct_t *lcm_struct = (lcm_struct_t*) g_ptr_array_index(lcmgen->structs, i);
        char* modfile_name = make_rust_mod_file_name(rust_path, lcm_struct);

        make_dirs_for_file(modfile_name);
        if (access(modfile_name, F_OK) != 0) {
            printf("Creating %s\n", modfile_name);
            FILE* f = fopen(modfile_name, "w");
            if (f == NULL) {
                printf("Couldn't open %s for writing\n", modfile_name);
                return -1;
            }

            emit_header_start(lcmgen, f);

            fclose(f);
        }

        free(modfile_name);
    }

    // Declare each struct
    for (unsigned int i = 0; i < g_ptr_array_size(lcmgen->structs); ++i) {
        lcm_struct_t* lcm_struct = (lcm_struct_t*) g_ptr_array_index(lcmgen->structs, i);
        printf("Emitting code for %s\n", lcm_struct->structname->lctypename);
        char* modfile_name = make_rust_mod_file_name(rust_path, lcm_struct);

        FILE* f = fopen(modfile_name, "a");
        if (f == NULL) {
            printf("Couldn't open %s for writing\n", modfile_name);
            return -1;
        }

        emit_struct_def(lcmgen, f, lcm_struct);

        fclose(f);
        free(modfile_name);
    }

    // Implement each struct
    for (unsigned int i = 0; i < g_ptr_array_size(lcmgen->structs); ++i) {
        lcm_struct_t* lcm_struct = (lcm_struct_t*) g_ptr_array_index(lcmgen->structs, i);
        printf("Implementing code for %s\n", lcm_struct->structname->lctypename);
        char* modfile_name = make_rust_mod_file_name(rust_path, lcm_struct);

        FILE* f = fopen(modfile_name, "a");
        if (f == NULL) {
            printf("Couldn't open %s for writing\n", modfile_name);
            return -1;
        }

        emit_impl_struct(lcmgen, f, lcm_struct);
        emit_impl_message(lcmgen, f, lcm_struct);

        fclose(f);
        free(modfile_name);
    }

    return 0;
}
