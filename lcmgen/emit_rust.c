#include <ctype.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
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

static char* make_rust_mod_file_name(const char* prefix, const lcm_struct_t* lcm_struct) {
    static const char* modfile_suffix = "/mod.rs";
    // allocate space for modfile name
    char* package_name = lcm_struct->structname->package;
    char* result = calloc(strlen(prefix) + 1 + strlen(package_name) + 1 + strlen(modfile_suffix), sizeof(char));
    if (result == NULL) {
        return NULL;
    }
    result[0] = '\0';

    // Build modfile path
    strcat(result, prefix);
    strcat(result, "/");
    strcat(result, package_name);
    for (char* c = result; *c != 0; ++c) {
        if (*c == '.') {
            *c = '/';
        }
    }
    strcat(result, modfile_suffix);
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
    // For type names following the C convention of *_t,
    // remove the _t suffix
    // (or rather, the trailing 'T', since we've already converted to camel case)
    if (result_char - result > 2 && *(result_char - 1) == 'T')
        *(result_char - 1) = 0;

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

static char *map_type_name(const lcm_typename_t *typename)
{
    const char *t = typename->shortname;

    if (!strcmp(t, "boolean"))
        return strdup("bool");

    if (!strcmp(t, "string"))
        return strdup("String");

    if (!strcmp(t, "byte"))
        return strdup("u8");

    if (!strcmp(t, "int8_t"))
        return strdup("i8");

    if (!strcmp(t, "int16_t"))
        return strdup("i16");

    if (!strcmp(t, "int32_t"))
        return strdup("i32");

    if (!strcmp(t, "int64_t"))
        return strdup("i64");

    if (!strcmp(t, "uint8_t"))
        return strdup("u8");

    if (!strcmp(t, "uint16_t"))
        return strdup("u16");

    if (!strcmp(t, "uint32_t"))
        return strdup("u32");

    if (!strcmp(t, "uint64_t"))
        return strdup("u64");

    if (!strcmp(t, "float"))
        return strdup("f32");

    if (!strcmp(t, "double"))
        return strdup("f64");

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
    emit(0, "use lcm::generic_array::{GenericArray, typenum};");
    emit(0, "use lcm;");
    emit(0, "use std::io::{Result, Error, ErrorKind, Read, Write};");
    emit(0, "");
    emit(0, "const MAX_VEC_SIZE : usize = 2000000;");
    emit(0, "");
}

static void emit_struct_def(lcmgen_t *lcmgen, FILE *f, lcm_struct_t *lcm_struct)
{
    char *struct_name = make_rust_type_name(lcm_struct->structname);

    emit(0, "#[derive(Debug, Default)]");
    emit(0, "pub struct %s {", struct_name);

    // Iterate over members of this struct
    // Arrays are represented by a Vec (for dynamically sized dimensions)
    // or by a GenericArray (for constant sized dimensions)
    for (unsigned int mind = 0; mind < g_ptr_array_size(lcm_struct->members); ++mind) {
        lcm_member_t* member = (lcm_member_t*) g_ptr_array_index(lcm_struct->members, mind);

        int ndim = g_ptr_array_size(member->dimensions);
        emit_start(1, "pub %s: ", member->membername);

        // Iterate forwards and open the array declaration
        for (unsigned int d = 0; d < ndim; ++d) {
            lcm_dimension_t *dimension = (lcm_dimension_t*) g_ptr_array_index(member->dimensions, d);
            switch (dimension->mode) {
            case LCM_CONST: {
                emit_continue("GenericArray<");
                /* emit_continue("["); */
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
                emit_continue(", typenum::U%s>", dimension->size);
                /* emit_continue("; %s]", dimension->size); */
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
    char *type_name = make_rust_type_name(lcm_struct->structname);

    emit(0, "impl %s {", type_name);
    emit(1, "pub fn new() -> Self {");
    emit(2, "Default::default()");
    emit(1, "}");
    emit(0, "}");
    emit(0, "");

    free(type_name);
}

static void emit_impl_message(lcmgen_t *lcmgen, FILE *f, lcm_struct_t *lcm_struct)
{
    char *type_name = make_rust_type_name(lcm_struct->structname);

    emit(0, "impl lcm::Message for %s {", type_name);

    emit(1,     "fn hash(&self) -> i64 {");
    emit(2,         "let hash = 0x%016"PRIx64";", lcm_struct->hash);
    emit(2,         "(hash << 1) + ((hash >> 63) & 1)");
    emit(1,     "}");

    emit(0, "");

    emit(1, "fn encode(&self, mut buffer: &mut Write) -> Result<()> {");
    for (unsigned int mind = 0; mind < g_ptr_array_size(lcm_struct->members); mind++) {
        lcm_member_t *member = (lcm_member_t *) g_ptr_array_index(lcm_struct->members, mind);
        emit(2, "self.%s.encode(&mut buffer)?;", member->membername);
    }
    emit(2, "Ok(())");
    emit(1, "}");

    emit(0, "");

    emit(1, "fn decode(&mut self, mut buffer: &mut Read) -> Result<()> {");
    for (unsigned int mind = 0; mind < g_ptr_array_size(lcm_struct->members); mind++) {
        lcm_member_t *member = (lcm_member_t *) g_ptr_array_index(lcm_struct->members, mind);

        // Arrays encodings are not length prefixed,
        // so if this is a dynamically sized array we need
        // to initialize it with the correct capacity.
        int ndim = g_ptr_array_size(member->dimensions);

        for (unsigned int d = 0; d != ndim; ++d) {
            lcm_dimension_t *dimension = (lcm_dimension_t *) g_ptr_array_index(member->dimensions, d);
            if (dimension->mode == LCM_VAR) {
                    emit(2, "let size_%d = self.%s as usize;", d, dimension->size);
            }
        }

        for (unsigned int d = 0; d != ndim; ++d) {
            lcm_dimension_t *dimension = (lcm_dimension_t *) g_ptr_array_index(member->dimensions, d);
            if (dimension->mode == LCM_VAR) {
                if (d == 0) {
                    emit(2, "self.%s = Vec::with_capacity(size_%d);", member->membername, d);
                } else {
                    emit(2+d, "*value_%d = Vec::with_capacity(size_%d);", d, d);
                }
            }

            if (d == 0) {
                emit(2, "for value_%d in self.%s.iter_mut() {", d+1, member->membername);
            } else {
                emit(2+d, "for value_%d in value_%d.iter_mut() {", d+1, d);
            }
            /*         for (unsigned int d_sub = 0; d_sub != d; ++d_sub) { */
            /*             emit(2+d_sub, "for v in self.%s.iter_mut() {", member->membername); */
            /*             emit(3+d_sub, "*v = Vec::with_capacity(size_%d);", d); */
            /*             emit(2+d_sub, "}"); */
            /*         } */
            /*     } */
            /* } */
        }

        if (ndim == 0) {
            emit(2, "self.%s.decode(&mut buffer)?;", member->membername);
        } else {
            emit(2+ndim, "value_%d.decode(&mut buffer)?;", ndim);
        }

        for (unsigned int d = ndim; d-- > 0; ) {
            emit(2+d, "}");
        }

#if 0
        if (ndim == 0) {
            // Nothing to do; this is not an array.
        } else {
            lcm_dimension_t *dimension = (lcm_dimension_t *) g_ptr_array_index(member->dimensions, 0);
            if (lcm_is_constant_size_array(member)) {
                // Nothing to do; array size is know at compile time.
            } else {
                emit(2, "let len = self.%s as usize;", dimension->size);
                emit(2, "if len > MAX_VEC_SIZE { return Err(Error::new(ErrorKind::Other, \"Array size too large\")); }");
                emit(2, "self.%s = Vec::with_capacity(len as usize);", member->membername);
            }
        }
#endif

        // emit(2, "self.%s.decode(&mut buffer)?;", member->membername);
    }
    emit(2, "Ok(())");
    // emit(2, ")");
    // emit(2, "Err(Error::new(ErrorKind::Other, \"Unimplemented\"))");
    emit(1, "}");

    emit(0, "");

    emit(1, "fn size(&self) -> usize {");
    emit(2, "0");
    for (unsigned int mind = 0; mind < g_ptr_array_size(lcm_struct->members); mind++) {
        lcm_member_t *lm = (lcm_member_t *) g_ptr_array_index(lcm_struct->members, mind);
        char *mn = lm->membername;
        emit(2, "+ self.%s.size()", mn);
    }
    emit(1, "}");

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

        emit_impl_message(lcmgen, f, lcm_struct);

        fclose(f);
        free(modfile_name);
    }

    return 0;
}
