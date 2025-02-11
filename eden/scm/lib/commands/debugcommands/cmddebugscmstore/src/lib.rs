/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

use std::collections::HashMap;
use std::io::Write;

use async_runtime::block_on;
use async_runtime::stream_to_iter as block_on_stream;
use clidispatch::abort;
use clidispatch::abort_if;
use clidispatch::errors;
use clidispatch::ReqCtx;
use cmdutil::define_flags;
use cmdutil::Config;
use cmdutil::Error;
use cmdutil::Result;
use cmdutil::IO;
use manifest::FileMetadata;
use manifest::FsNodeMetadata;
use manifest::Manifest;
use repo::repo::Repo;
use revisionstore::scmstore::file_to_async_key_stream;
use revisionstore::scmstore::FileAttributes;
use revisionstore::scmstore::FileStoreBuilder;
use revisionstore::scmstore::TreeStoreBuilder;
use types::fetch_mode::FetchMode;
use types::Key;
use types::RepoPathBuf;

define_flags! {
    pub struct DebugScmStoreOpts {
        /// Fetch mode (file or tree)
        mode: String,

        /// Input file containing keys to fetch (hgid,path separated by newlines)
        requests_file: Option<String>,

        /// Only check for the entity locally, don't make a remote request
        local: bool,

        /// Only fetch AUX data (don't request file content).
        aux_only: bool,

        /// Revision for positional file paths.
        #[short('r')]
        #[argtype("REV")]
        rev: Option<String>,

        #[args]
        args: Vec<String>,
    }
}

#[derive(PartialEq)]
enum FetchType {
    File,
    Tree,
}

pub fn run(ctx: ReqCtx<DebugScmStoreOpts>, repo: &mut Repo) -> Result<u8> {
    let mode = match ctx.opts.mode.as_ref() {
        "file" => FetchType::File,
        "tree" => FetchType::Tree,
        _ => return Err(errors::Abort("'mode' must be one of 'file' or 'tree'".into()).into()),
    };

    abort_if!(
        ctx.opts.requests_file.is_some() == ctx.opts.rev.is_some(),
        "must specify exactly one of --rev or --path"
    );

    abort_if!(
        ctx.opts.rev.is_some() && mode == FetchType::Tree,
        "--rev doesn't support trees yet",
    );

    let keys: Vec<Key> = if let Some(path) = ctx.opts.requests_file {
        block_on_stream(block_on(file_to_async_key_stream(path.into()))?).collect()
    } else {
        let wc = repo.working_copy()?;
        let commit = repo.resolve_commit(Some(&wc.treestate().lock()), &ctx.opts.rev.unwrap())?;
        let manifest = repo.tree_resolver()?.get(&commit)?;
        ctx.opts
            .args
            .into_iter()
            .map(|path| {
                let path = RepoPathBuf::from_string(path)?;
                match manifest.get(&path)? {
                    None => abort!("path {path} not in manifest"),
                    Some(FsNodeMetadata::Directory(_)) => abort!("path {path} is a directory"),
                    Some(FsNodeMetadata::File(FileMetadata { hgid, .. })) => {
                        Ok(Key::new(path, hgid))
                    }
                }
            })
            .collect::<Result<_>>()?
    };

    let config = repo.config();

    match mode {
        FetchType::File => fetch_files(
            &ctx.core.io,
            config,
            keys,
            ctx.opts.local,
            ctx.opts.aux_only,
        )?,
        FetchType::Tree => fetch_trees(&ctx.core.io, config, keys, ctx.opts.local)?,
    }

    Ok(0)
}

fn fetch_files(
    io: &IO,
    config: &dyn Config,
    keys: Vec<Key>,
    local: bool,
    aux_only: bool,
) -> Result<()> {
    let file_builder = FileStoreBuilder::new(&config);
    let store = file_builder.build()?;

    let mut stdout = io.output();

    let fetch_mode = if local {
        FetchMode::LocalOnly
    } else {
        FetchMode::AllowRemote
    };

    let mut fetch_and_display_successes =
        |keys: Vec<Key>, attrs: FileAttributes| -> HashMap<Key, Vec<Error>> {
            let fetch_result = store.fetch(keys.into_iter(), attrs, fetch_mode);

            let (found, missing, _errors) = fetch_result.consume();
            for (_, file) in found.into_iter() {
                let _ = write!(stdout, "Successfully fetched file: {:#?}\n", file);
            }

            missing
        };

    let mut missing = fetch_and_display_successes(
        keys,
        FileAttributes {
            content: !aux_only,
            aux_data: true,
        },
    );

    if !aux_only {
        // Maybe we failed because only one of content or aux data is available.
        // The API doesn't let us say "aux data if present", so try each separately.
        missing = fetch_and_display_successes(
            missing.into_keys().collect(),
            FileAttributes {
                content: true,
                aux_data: false,
            },
        );
        missing = fetch_and_display_successes(
            missing.into_keys().collect(),
            FileAttributes {
                content: false,
                aux_data: true,
            },
        );
    }

    for (key, errors) in missing.into_iter() {
        write!(
            stdout,
            "Failed to fetch file: {key:#?}\nError: {errors:?}\n"
        )?;
    }

    Ok(())
}

fn fetch_trees(io: &IO, config: &dyn Config, keys: Vec<Key>, local: bool) -> Result<()> {
    let mut tree_builder = TreeStoreBuilder::new(config);
    tree_builder = tree_builder.suffix("manifests");
    let store = tree_builder.build()?;

    let mut stdout = io.output();

    let fetch_mode = if local {
        FetchMode::LocalOnly
    } else {
        FetchMode::AllowRemote
    };

    let fetch_result = store.fetch_batch(keys.into_iter(), fetch_mode);

    let (found, missing, _errors) = fetch_result.consume();
    for complete in found.into_iter() {
        write!(stdout, "Successfully fetched tree: {:#?}\n", complete)?;
    }
    for incomplete in missing.into_iter() {
        write!(stdout, "Failed to fetch tree: {:#?}\n", incomplete)?;
    }

    Ok(())
}

pub fn aliases() -> &'static str {
    "debugscmstore"
}

pub fn doc() -> &'static str {
    "test file and tree fetching using scmstore"
}

pub fn synopsis() -> Option<&'static str> {
    None
}
