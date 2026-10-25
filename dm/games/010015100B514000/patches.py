# Super Mario Bros. Wonder (010015100B514000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # Super Mario Bros. Wonder v0 main
  'cd6e42aee7934f4d8393cd43893ae25e': [
    # Resource heap slack: each resource unit's ExpHeap is created with the resource size table value + 0x1008 (main+0x1e603c → ExpHeap::create main+0x4aac38); the table holds Nintendo's measured peaks with almost no margin
    # (PlayerYoshiAnm uses 99%). On the host, Model/NpcCommonerA.bfres in the opening course Course900 is about 0x70 bytes short: after the model resource's last GPU chunk (MatBlock,
    # 0x9000 bytes, 4 KB aligned, main+0x4290c0) is placed, its 0x598-byte descriptor does not fit, the GPU allocation fails → a buffer is built on a null memory pool → null read crash at main+0x106f5c8.
    # Where this differs from hardware is not known (every allocation size before this block is determined by game code and data); for now every resource heap gets +16 KB.
    (0x1e62fc, 'e00317aa', 'e0124091', 'resource heap size: mov x0,x23 → add x0,x23,#0x4000 (ExpHeap::create size argument +16 KB)'),
  ],
  # Super Mario Bros. Wonder 1.0.1 (v65536), main f91868b8...: resource heap slack as above.
  # Ported from v0 by the unique address-masked match of the complete 1,420-byte function
  # 0x1e603c -> 0x1f65d0; both ExpHeap::create call sites also map to the same callee
  # (0x4aac38 -> 0x48b134). The original instruction is asserted by the rewriter.
  'f91868b88f60d3d59009db3389fde314': [
    (0x1f6890, 'e00317aa', 'e0124091', 'resource heap size: mov x0,x23 -> add x0,x23,#0x4000 (ExpHeap::create size argument +16 KB)'),
  ],
  # Super Mario Bros. Wonder 1.2.1 (v327680) main: same as above. Location carried over from v0 by dm/tools/nso_fnmap.py (containing function 0x1e603c → 0x1cf6f0 identical after address masking, ExpHeap::create 0x4aac38 → 0x4c4a18)
  'ff773e90972d544eb79406eaa65396d5': [
    (0x1cf9b0, 'e00317aa', 'e0124091', 'resource heap size: mov x0,x23 → add x0,x23,#0x4000 (ExpHeap::create size argument +16 KB)'),
  ],
}

# Unknown main builds: identify the complete heap setup function and its direct
# callees/imports, not an address or the short mov/add sequence. Verified against
# v0 (0x1e603c) and 1.2.1 (0x1cf6f0); a helper changed between them, so each has its
# own fingerprint. Known builds above keep their exact original-byte assertions.
MATCH_CUTS = {'main': [{
  'name': 'resource heap slack',
  'size': 1420,
  'fingerprints': [
    '8062801c559542f56d41eda82685f85f0302933458a57fca074e9b3a3a475102',
    '09233b021587d058cf645d301d58d07e3dc2bdbac1737b874bb7a0ba914725ed',
  ],
  'cuts': [(0x2c0, 'e00317aa', 'e0124091', 'resource heap size: mov x0,x23 -> add x0,x23,#0x4000 (ExpHeap::create size argument +16 KB)')],
}]}
