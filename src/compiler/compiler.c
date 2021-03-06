// Copyright (c) 2019 Christoffer Lerno. All rights reserved.
// Use of this source code is governed by the GNU LGPLv3.0 license
// a copy of which can be found in the LICENSE file.

#include "compiler_internal.h"
#include "../build/build_options.h"

Compiler compiler;

void compiler_init(void)
{
	stable_init(&compiler.modules, 64);
	stable_init(&compiler.global_symbols, 0x1000);
}

static void compiler_lex(BuildTarget *target)
{
	VECEACH(target->sources, i)
	{
		bool loaded = false;
		File *file = source_file_load(target->sources[i], &loaded);
		if (loaded) continue;
		Lexer lexer;
		lexer_init_with_file(&lexer, file);
		printf("# %s\n", file->full_path);
		while (1)
		{
			Token token = lexer_scan_token(&lexer);
			printf("%s ", token_type_to_string(token.type));
			if (token.type == TOKEN_EOF) break;
		}
		printf("\n");
	}
	exit(EXIT_SUCCESS);
}

void compiler_parse(BuildTarget *target)
{
	VECEACH(target->sources, i)
	{
		bool loaded = false;
		File *file = source_file_load(target->sources[i], &loaded);
		if (loaded) continue;
		diag_reset();
		Context *context = context_create(file, target);
		parse_file(context);
		context_print_ast(context, stdout);
	}
	exit(EXIT_SUCCESS);
}

void compiler_compile(BuildTarget *target)
{
	Context **contexts = NULL;
	VECEACH(target->sources, i)
	{
		bool loaded = false;
		File *file = source_file_load(target->sources[i], &loaded);
		if (loaded) continue;
		diag_reset();
		Context *context = context_create(file, target);
		vec_add(contexts, context);
		parse_file(context);
	}
	/*
	const char *printf = "printf";
	TokenType t_type = TOKEN_IDENT;
	const char *interned = symtab_add(printf, (uint32_t) 6, fnv1a(printf, (uint32_t)6), &t_type);
	Decl *decl = decl_new(DECL_FUNC, wrap(interned), VISIBLE_PUBLIC);
	Type *type = type_new(TYPE_POINTER);
	type->base = type_char;
	sema_resolve_type(contexts[0], type);
	Decl *param = decl_new_var(wrap("str"), type, VARDECL_PARAM, VISIBLE_LOCAL);
	vec_add(decl->func.function_signature.params, param);
	decl->func.function_signature.rtype = type_void;
	decl->resolve_status = RESOLVE_DONE;
	context_register_global_decl(contexts[0], decl);
*/
	assert(contexts);
	VECEACH(contexts, i)
	{
		sema_analysis_pass_process_imports(contexts[i]);
	}
	if (diagnostics.errors > 0) exit(EXIT_FAILURE);

	VECEACH(contexts, i)
	{
		sema_analysis_pass_conditional_compilation(contexts[i]);
	}
	if (diagnostics.errors > 0) exit(EXIT_FAILURE);

	VECEACH(contexts, i)
	{
		sema_analysis_pass_decls(contexts[i]);
	}
	if (diagnostics.errors > 0) exit(EXIT_FAILURE);

	llvm_codegen_setup();
	VECEACH(contexts, i)
	{
		Context *context = contexts[i];
		llvm_codegen(context);
	}
	exit(EXIT_SUCCESS);
}

static void target_expand_source_names(BuildTarget *target)
{
	const char **files = NULL;
	VECEACH(target->sources, i)
	{
		const char *name = target->sources[i];
		size_t name_len = strlen(name);
		if (name_len < 1) goto INVALID_NAME;
		if (name[name_len - 1] == '*')
		{
			if (name_len == 1 || name[name_len - 2] == '/')
			{
				char *path = strdup(name);
				path[name_len - 1] = '\0';
				file_add_wildcard_files(&files, path, false);
				free(path);
				continue;
			}
			if (name[name_len - 2] != '*') goto INVALID_NAME;
			if (name_len == 2 || name[name_len - 3] == '/')
			{
				char *path = strdup(name);
				path[name_len - 2] = '\0';
				file_add_wildcard_files(&files, path, true);
				free(path);
				continue;
			}
			goto INVALID_NAME;
		}
		if (name_len < 4) goto INVALID_NAME;
		if (strcmp(&name[name_len - 3], ".c3") != 0) goto INVALID_NAME;
		vec_add(files, name);
		continue;
		INVALID_NAME:
		error_exit("File names must end with .c3 or they cannot be compiled: '%s' is invalid.", name);
	}
	target->sources = files;
}

void compile_files(BuildTarget *target)
{
	if (!target)
	{
		target = CALLOCS(BuildTarget);
		target->type = TARGET_TYPE_EXECUTABLE;
		target->sources = build_options.files;
		target->name = "a.out";
	}
	target_expand_source_names(target);
	target_setup();

	if (!vec_size(target->sources)) error_exit("No files to compile.");
	switch (build_options.compile_option)
	{
		case COMPILE_LEX_ONLY:
			compiler_lex(target);
			break;
		case COMPILE_LEX_PARSE_ONLY:
			compiler_parse(target);
			break;
		default:
			compiler_compile(target);
			break;
	}
	TODO
}


Decl *compiler_find_symbol(Token token)
{
	return stable_get(&compiler.global_symbols, token.string);
}

void compiler_add_type(Type *type)
{
	DEBUG_LOG("Created type %s.", type->name);
	assert(type_ok(type));
	VECADD(compiler.type, type);
}
Module *compiler_find_or_create_module(Path *module_name)
{
	Module *module = stable_get(&compiler.modules, module_name->module);

	if (module)
	{
		// We might have gotten an auto-generated module, if so
		// update the path here.
		if (module->name->span.loc == INVALID_LOC && module_name->span.loc != INVALID_LOC)
		{
			module->name = module_name;
		}
		return module;
	}

	DEBUG_LOG("Creating module %s.", module_name->module);
	// Set up the module.
	module = CALLOCS(Module);
	module->name = module_name;
	stable_init(&module->symbols, 0x10000);
	stable_set(&compiler.modules, module_name->module, module);
	// Now find the possible parent array:
	Path *parent_path = path_find_parent_path(module_name);
	if (parent_path)
	{
		// Get the parent
		Module *parent_module = compiler_find_or_create_module(parent_path);
		vec_add(parent_module->sub_modules, module);
	}
	return module;
}

void compiler_register_public_symbol(Decl *decl)
{
	Decl *prev = stable_get(&compiler.global_symbols, decl->name);
	// If the previous symbol was already declared globally, remove it.
	stable_set(&compiler.global_symbols, decl->name, prev ? poisoned_decl : decl);
	STable *sub_module_space = stable_get(&compiler.qualified_symbols, decl->module->name->module);
	if (!sub_module_space)
	{
		sub_module_space = malloc_arena(sizeof(*sub_module_space));
		stable_init(sub_module_space, 0x100);
		stable_set(&compiler.qualified_symbols, decl->module->name->module, sub_module_space);
	}
	prev = stable_get(sub_module_space, decl->name);
	stable_set(sub_module_space, decl->name, prev ? poisoned_decl : decl);
}
