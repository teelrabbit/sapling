#debugruntest-compatible

  $ eagerepo

  $ newrepo server
  $ drawdag <<EOS
  > B
  > |
  > A
  > EOS

  $ newclientrepo client test:server

First, sanity that we don't have any data locally:
  $ hg debugscmstore -r $A A --local --mode=file
  abort: unknown revision '426bada5c67598ca65036d57d9e4b64b0c1ce7a0'
  [255]

  $ hg prefetch -q -r $A

Now we do have aux data locally:
  $ hg debugscmstore -r $A A --local --mode=file --config scmstore.compute-aux-data=false
  Successfully fetched file: StoreFile {
      content: Some(
          IndexedLog(
              Entry {
                  key: Key {
                      path: RepoPathBuf(
                          "A",
                      ),
                      hgid: HgId("005d992c5dcf32993668f7cede29d296c494a5d9"),
                  },
                  metadata: Metadata {
                      size: None,
                      flags: None,
                  },
                  content: OnceCell(Uninit),
                  compressed_content: Some(
                      b"\x01\x00\x00\x00\x10A",
                  ),
              },
          ),
      ),
      aux_data: Some(
          FileAuxData {
              total_size: 1,
              content_id: ContentId("eb56488e97bb4cf5eb17f05357b80108a4a71f6c3bab52dfcaec07161d105ec9"),
              sha1: Sha1("6dcd4ce23d88e2ee9568ba546c007c63d9131c1b"),
              sha256: Sha256("559aead08264d5795d3909718cdd05abd49572e84fe55590eef31a88a08fdffd"),
              seeded_blake3: Some(
                  Blake3("5ad3ba58a716e5fc04296ac9af7a1420f726b401fdf16d270beb5b6b30bc0cda"),
              ),
          },
      ),
  }


Fetch only content first:
  $ hg cat -q -r $B B
  B (no-eol)

Make sure we don't have aux data yet:
  $ hg debugscmstore -r $B B --local --mode=file --config scmstore.compute-aux-data=false
  Successfully fetched file: StoreFile {
      content: Some(
          IndexedLog(
              Entry {
                  key: Key {
                      path: RepoPathBuf(
                          "B",
                      ),
                      hgid: HgId("35e7525ce3a48913275d7061dd9a867ffef1e34d"),
                  },
                  metadata: Metadata {
                      size: None,
                      flags: None,
                  },
                  content: OnceCell(Uninit),
                  compressed_content: Some(
                      b"\x01\x00\x00\x00\x10B",
                  ),
              },
          ),
      ),
      aux_data: None,
  }

Fetching only aux data does not trigger a remote query:
  $ LOG=eagerepo::api=debug hg debugscmstore -r $B B --aux-only --mode=file
  Successfully fetched file: StoreFile {
      content: None,
      aux_data: Some(
          FileAuxData {
              total_size: 1,
              content_id: ContentId("55662471e2a28db8257939b2f9a2d24e65b46a758bac12914a58f17dcde6905f"),
              sha1: Sha1("ae4f281df5a5d0ff3cad6371f76d5c29b6d953ec"),
              sha256: Sha256("df7e70e5021544f4834bbee64a9e3789febc4be81470df629cad6ddb03320a5c"),
              seeded_blake3: Some(
                  Blake3("5667f2421ac250c4bb9af657b5ead3cdbd940bfbc350b2bfee47454643832b48"),
              ),
          },
      ),
  }
