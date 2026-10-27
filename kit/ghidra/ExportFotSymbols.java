// MIT licensed (see ../LICENSE).
//
// Regenerates the symbol files of the Fallout Tactics reverse-engineering kit
// (symbols/functions.tsv, data.tsv, types.h and the two reports) from a Ghidra
// project, and checks every exported name against the executables players own.
//
// This is the maintainers' tool. Contributors only need ApplyFotSymbols.java.
//
// What it reads
//   The open program (a Ghidra database of BOS.exe or BOS_HR.exe, or one derived from them)
//   and two reference executables:
//     BOS.exe     (Steam and GOG 1.27)  SHA-256 c5b91fe5...
//     BOS_HR.exe  (Steam, hi-res build) SHA-256 ae7ddade...
//   The script refuses to run if either file has a different hash.
//
// What it writes (into <outDir>)
//   functions.tsv             named functions verified for at least one executable
//   functions-unverified.tsv  named functions verified for neither executable
//   data.tsv                  named global data
//   types.h                   user-defined structs, unions, enums and typedefs, as C
//   verification.txt          where the program's bytes differ from each executable
//   Nothing else is written. No comments, decompiler text, disassembly, strings
//   or bytes from the game are copied: only names, addresses, sizes, signatures,
//   type layouts, and the confidence bucket of each AI-made name.
//
// How a name is classified (column "source")
//   ftse           Ghidra symbol source USER_DEFINED: names people wrote in FTSE's database.
//                  (Names you type by hand in your own database are USER_DEFINED too and
//                  come out as ftse as well; Ghidra cannot tell who typed a name.)
//   deterministic  source ANALYSIS: names derived from RTTI, vtables and log strings
//                  (Ghidra's own analyzer names are left out: Unwind@..., Catch@...,
//                  names the Function ID analyzer made)
//   agent          source IMPORTED and a "[agent conf=...]" tag in the plate comment
//   Comment text is read in two places only, and never copied: the agent tag, and the
//   "Library Function" note that marks a name Ghidra's Function ID analyzer made.
//   Default names (FUN_..., thunk_...) are not exported.
//
// How a name is verified (columns "verified_bos" and "verified_bos_hr")
//   A function is verified for an executable when every byte of its body in the
//   program is identical to the same address in that executable. The file offset
//   comes from the executable's PE section table. Both executables and the
//   program are linked at 0x400000, so no relocation masking is needed.
//   Data is verified when its bytes are identical (initialized data) or, for
//   uninitialized data, when a verified function references it.
//
// Usage (headless)
//   analyzeHeadless <projectDir> <ProjectName> -process <program> -noanalysis -readOnly \
//     -scriptPath <kit>/ghidra -postScript ExportFotSymbols.java <outDir> <BOS.exe> <BOS_HR.exe>
//
//@category FOT

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.data.Enum;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import ghidra.util.task.TaskMonitor;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.regex.*;

public class ExportFotSymbols extends GhidraScript {

    // ---- the executables the kit targets -------------------------------------------------
    static final String[] EXE_NAME = { "BOS.exe", "BOS_HR.exe" };
    static final String[] EXE_SHA256 = {
        "c5b91fe56ef8de118ce22ccd5e6512f217ca16c8b8758ea14c626229eb5a4e40",
        "ae7ddade7f3c5c48552b54f72b795b3919477b789bba7c910db46c8e75c812d0" };

    // Agent confidence tags hold a number from 0.3 to 0.95 (or already high/medium/low).
    // The agents were told: 0.9+ only when unambiguous, 0.5-0.8 plausible, below 0.5 a guess.
    static final double HIGH_AT = 0.85;
    static final double MEDIUM_AT = 0.5;

    // Types Ghidra's loader and analyzers create in any import (PE headers, RTTI, exception
    // handling, the thread block). A contributor's own analysis recreates them, so they are
    // left out unless an exported signature or data item needs them.
    static final String[] GENERATED_CATEGORIES = { "/PE", "/DOS", "/Demangler", "/auto_structs" };
    static final Set<String> GENERATED_TYPES = new HashSet<>(Arrays.asList(
        "FuncInfo", "HandlerType", "TryBlockMapEntry", "UnwindMapEntry",
        "_s_FuncInfo", "_s_HandlerType", "_s_TryBlockMapEntry", "_s_UnwindMapEntry",
        "RTTIBaseClassDescriptor", "RTTIClassHierarchyDescriptor", "RTTICompleteObjectLocator",
        "_s__RTTIBaseClassDescriptor", "_s__RTTIClassHierarchyDescriptor",
        "_s__RTTICompleteObjectLocator", "TypeDescriptor", "CLIENT_ID",
        "IMAGE_RESOURCE_DIRECTORY_ENTRY_DirectoryStruct",
        "IMAGE_RESOURCE_DIRECTORY_ENTRY_DirectoryUnion"));

    static final Pattern AGENT_TAG = Pattern.compile("\\[agent conf=([0-9]*\\.?[0-9]+|high|medium|low)\\]");

    // ---- PE reader -------------------------------------------------------------------------
    static class Section { String name; long rva, vsize, raw, rawSize; }

    static class Exe {
        String name, path, sha;
        byte[] data;
        long imageBase, sizeOfImage, headersSize;
        List<Section> sections = new ArrayList<>();
        AddressSet diff = new AddressSet();       // program bytes that differ from this file
        long diffBytes;

        // file offset of an RVA, or -1 when the file does not hold that byte
        long fileOffset(long rva) {
            if (rva >= 0 && rva < headersSize && rva < data.length) return rva;
            for (Section s : sections) {
                if (rva >= s.rva && rva < s.rva + s.rawSize) {
                    long fo = s.raw + (rva - s.rva);
                    return fo < data.length ? fo : -1;
                }
            }
            return -1;
        }

        // how many bytes from rva on are held by the file in one piece
        long backedRemaining(long rva) {
            if (rva >= 0 && rva < headersSize) return Math.min(headersSize - rva, data.length - rva);
            for (Section s : sections) {
                if (rva >= s.rva && rva < s.rva + s.rawSize) {
                    long fo = s.raw + (rva - s.rva);
                    return Math.min(s.rva + s.rawSize - rva, data.length - fo);
                }
            }
            return 0;
        }

        // the byte the loader would put at rva: the file's byte, 0 for zero-filled memory
        // (a section's virtual size is larger than its raw size), or -1 when nothing is there
        int expectedByte(long rva) {
            long fo = fileOffset(rva);
            if (fo >= 0) return data[(int) fo] & 0xff;
            for (Section s : sections) if (rva >= s.rva + s.rawSize && rva < s.rva + s.vsize) return 0;
            return -1;
        }

        // [rva, rva+len) is held entirely by the file
        boolean fileBacked(long rva, long len) { return len > 0 && backedRemaining(rva) >= len; }

        // [rva, rva+len) lies entirely in zero-filled memory (uninitialized data): the file has no bytes to compare
        boolean zeroFilled(long rva, long len) {
            for (Section s : sections) if (rva >= s.rva + s.rawSize && rva + len <= s.rva + s.vsize) return true;
            return false;
        }
    }

    static int le16(byte[] d, int o) { return (d[o] & 0xff) | (d[o + 1] & 0xff) << 8; }
    static long le32(byte[] d, int o) {
        return ((d[o] & 0xffL) | (d[o + 1] & 0xffL) << 8 | (d[o + 2] & 0xffL) << 16 | (d[o + 3] & 0xffL) << 24);
    }

    Exe readExe(int index, String path) throws Exception {
        Exe x = new Exe();
        x.name = EXE_NAME[index];
        x.path = path;
        x.data = Files.readAllBytes(Paths.get(path));
        x.sha = hex(MessageDigest.getInstance("SHA-256").digest(x.data));
        if (!x.sha.equals(EXE_SHA256[index])) {
            throw new IllegalStateException(path + " is not " + x.name + " (SHA-256 " + x.sha + ", expected " + EXE_SHA256[index] + ")");
        }
        byte[] d = x.data;
        int pe = (int) le32(d, 0x3c);
        if (d[pe] != 'P' || d[pe + 1] != 'E') throw new IllegalStateException(x.name + ": no PE signature");
        int nsec = le16(d, pe + 6);
        int optSize = le16(d, pe + 20);
        int opt = pe + 24;
        if (le16(d, opt) != 0x10b) throw new IllegalStateException(x.name + ": not a PE32 image");
        x.imageBase = le32(d, opt + 28);
        x.sizeOfImage = le32(d, opt + 56);
        x.headersSize = le32(d, opt + 60);
        int st = opt + optSize;
        for (int i = 0; i < nsec; i++) {
            int o = st + 40 * i;
            Section s = new Section();
            int n = 0;
            while (n < 8 && d[o + n] != 0) n++;
            s.name = new String(d, o, n, StandardCharsets.US_ASCII);
            s.vsize = le32(d, o + 8);
            s.rva = le32(d, o + 12);
            s.rawSize = le32(d, o + 16);
            s.raw = le32(d, o + 20);
            x.sections.add(s);
        }
        return x;
    }

    static String hex(byte[] b) {
        StringBuilder sb = new StringBuilder();
        for (byte v : b) sb.append(String.format("%02x", v & 0xff));
        return sb.toString();
    }

    // ---- program vs executable --------------------------------------------------------------
    AddressSet imageInitialized = new AddressSet();   // initialized bytes of the program inside the PE image
    long programBase;

    void computeDiff(Exe x) throws Exception {
        Memory mem = currentProgram.getMemory();
        AddressSpace space = currentProgram.getAddressFactory().getDefaultAddressSpace();
        for (MemoryBlock b : mem.getBlocks()) {
            if (b.isOverlay() || b.isExternalBlock() || !b.isInitialized()) continue;
            long bs = b.getStart().getOffset();
            if (bs < programBase || bs >= programBase + x.sizeOfImage) continue;   // not part of the image (Ghidra's own blocks)
            byte[] pb = new byte[(int) b.getSize()];
            b.getBytes(b.getStart(), pb);
            long run = -1;
            for (int i = 0; i < pb.length; i++) {
                int want = x.expectedByte(bs - programBase + i);
                boolean same = want >= 0 && want == (pb[i] & 0xff);
                if (!same) {
                    if (run < 0) run = i;
                }
                else if (run >= 0) {
                    x.diff.add(space.getAddress(bs + run), space.getAddress(bs + i - 1));
                    x.diffBytes += i - run;
                    run = -1;
                }
            }
            if (run >= 0) {
                x.diff.add(space.getAddress(bs + run), space.getAddress(bs + pb.length - 1));
                x.diffBytes += pb.length - run;
            }
        }
    }

    void collectImageMemory(Exe any) {
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (b.isOverlay() || b.isExternalBlock() || !b.isInitialized()) continue;
            long bs = b.getStart().getOffset();
            if (bs < programBase || bs >= programBase + any.sizeOfImage) continue;
            imageInitialized.add(b.getStart(), b.getEnd());
        }
    }

    // every byte of the body is initialized program memory in the image and equals the file's byte
    boolean bytesMatch(AddressSetView body, Exe x) {
        return imageInitialized.contains(body) && !x.diff.intersects(body);
    }

    // ---- rows -------------------------------------------------------------------------------
    static class FnRow {
        Function f;
        Address addr;
        long size;
        String name, sig = "", source, conf = "";
        boolean[] ok = new boolean[2];
        long[] badBytes = new long[2];
    }

    static class DataRow {
        Address addr;
        long size;
        String name, type, source = "ftse";
        boolean[] ok = new boolean[2];
    }

    final Map<String,Integer> skipped = new TreeMap<>();
    final Map<String,String> renamedTypes = new TreeMap<>();   // type names that were not valid C -> the name used in types.h
    void skip(String why) { skipped.merge(why, 1, Integer::sum); }

    static String clean(String s) {
        return s.replace('\t', ' ').replace('\r', ' ').replace('\n', ' ');
    }

    String bucket(String raw) {
        if (raw.equals("high") || raw.equals("medium") || raw.equals("low")) return raw;
        double c = Double.parseDouble(raw);
        return c >= HIGH_AT ? "high" : c >= MEDIUM_AT ? "medium" : "low";
    }

    // returns the source ("ftse", "deterministic", "agent") or null when the function is not exported
    String classify(Function f, String[] confOut) {
        SourceType ss = f.getSymbol().getSource();
        String nm = f.getName();
        if (ss == SourceType.DEFAULT) { skip("default name (FUN_..., thunk_...)"); return null; }
        if (nm.startsWith("Unwind@") || nm.startsWith("Catch@")) { skip("Ghidra exception-handler funclet"); return null; }
        String cmt = f.getComment();
        if (ss == SourceType.USER_DEFINED) return "ftse";
        if (ss == SourceType.ANALYSIS) {
            if (cmt != null && cmt.startsWith("Library Function")) { skip("name from Ghidra's Function ID analyzer"); return null; }
            return "deterministic";
        }
        if (ss == SourceType.IMPORTED) {
            Matcher m = cmt == null ? null : AGENT_TAG.matcher(cmt);
            if (m != null && m.find()) { confOut[0] = bucket(m.group(1)); return "agent"; }
            skip("imported by the loader (no agent tag)");
            return null;
        }
        skip("other source " + ss);
        return null;
    }

    // ---- types ------------------------------------------------------------------------------
    static DataType stripToBase(DataType d) {
        while (d != null) {
            if (d instanceof Pointer) d = ((Pointer) d).getDataType();
            else if (d instanceof Array) d = ((Array) d).getDataType();
            else break;
        }
        return d;
    }

    boolean programOwned(DataType d) {
        SourceArchive sa = d.getSourceArchive();
        return sa == null || sa.getArchiveType() == ArchiveType.PROGRAM;
    }

    boolean generated(DataType d) {
        String cp = d.getCategoryPath().getPath();
        for (String g : GENERATED_CATEGORIES) if (cp.equals(g) || cp.startsWith(g + "/")) return true;
        return GENERATED_TYPES.contains(d.getName());
    }

    // add a type (and everything it needs) to the set of types to write; mirrors what DataTypeWriter writes
    void needType(DataType d, Set<DataType> out, Set<DataType> seen) {
        DataType b = stripToBase(d);
        if (b == null || !seen.add(b)) return;
        if (b instanceof TypeDef) {
            out.add(b);
            needType(((TypeDef) b).getDataType(), out, seen);
        }
        else if (b instanceof Composite) {
            out.add(b);
            for (DataTypeComponent c : ((Composite) b).getComponents()) needType(c.getDataType(), out, seen);
        }
        else if (b instanceof Enum) {
            out.add(b);
        }
        else if (b instanceof FunctionDefinition) {
            FunctionDefinition fd = (FunctionDefinition) b;
            needType(fd.getReturnType(), out, seen);
            for (ParameterDefinition p : fd.getArguments()) needType(p.getDataType(), out, seen);
        }
    }

    static String cIdent(String n) {
        String s = n.replace("::", "__");
        s = s.replaceAll("[^A-Za-z0-9_]", "_");
        if (s.isEmpty() || Character.isDigit(s.charAt(0))) s = "_" + s;
        return s;
    }

    // ---- main -------------------------------------------------------------------------------
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (currentProgram == null || args.length != 3) {
            println("usage: ExportFotSymbols.java <outDir> <BOS.exe> <BOS_HR.exe>   (with a program open)");
            return;
        }
        Path outDir = Paths.get(args[0]);
        Files.createDirectories(outDir);
        Exe[] exe = { readExe(0, args[1]), readExe(1, args[2]) };
        Program p = currentProgram;
        programBase = p.getImageBase().getOffset();
        for (Exe x : exe) {
            if (x.imageBase != programBase) throw new IllegalStateException(x.name + " is linked at " + Long.toHexString(x.imageBase) + ", the program at " + Long.toHexString(programBase));
        }
        collectImageMemory(exe[0]);
        for (Exe x : exe) computeDiff(x);
        for (Exe x : exe) {
            println("VERIFY " + x.name + ": program differs in " + x.diffBytes + " bytes, " + x.diff.getNumAddressRanges() + " ranges");
        }

        FunctionManager fm = p.getFunctionManager();
        Listing listing = p.getListing();

        // ---- functions ----
        List<FnRow> rows = new ArrayList<>();
        for (Function f : fm.getFunctions(true)) {
            String[] conf = { "" };
            String src = classify(f, conf);
            if (src == null) continue;
            FnRow r = new FnRow();
            r.f = f;
            r.addr = f.getEntryPoint();
            r.size = f.getBody().getNumAddresses();
            r.name = clean(f.getName(true));
            r.source = src;
            r.conf = conf[0];
            if (f.getSignatureSource() == SourceType.USER_DEFINED) {
                if (f.hasCustomVariableStorage()) skip("signature not exported (custom variable storage)");
                else r.sig = clean(f.getPrototypeString(false, true));
            }
            for (int i = 0; i < 2; i++) {
                r.ok[i] = bytesMatch(f.getBody(), exe[i]);
                if (!r.ok[i]) r.badBytes[i] = exe[i].diff.intersect(f.getBody()).getNumAddresses();
            }
            rows.add(r);
        }
        rows.sort(Comparator.comparing((FnRow r) -> r.addr));

        // ---- data ----
        List<DataRow> data = new ArrayList<>();
        for (Symbol s : p.getSymbolTable().getAllSymbols(false)) {
            if (s.getSymbolType() == SymbolType.FUNCTION || s.isExternal()) continue;
            if (s.getSource() != SourceType.USER_DEFINED) continue;
            Address a = s.getAddress();
            MemoryBlock b = p.getMemory().getBlock(a);
            if (b == null || b.isExecute()) { skip("label inside code"); continue; }
            Data d = listing.getDataAt(a);
            DataRow r = new DataRow();
            r.addr = a;
            r.name = clean(s.getName(true));
            r.type = d == null ? "undefined" : clean(d.getDataType().getDisplayName());
            r.size = d == null ? 1 : d.getLength();
            AddressSet range = new AddressSet(a, a.add(r.size - 1));
            long rva = a.getOffset() - programBase;
            for (int i = 0; i < 2; i++) {
                if (exe[i].fileBacked(rva, r.size)) {
                    // initialized data: the bytes must be the file's bytes
                    r.ok[i] = imageInitialized.contains(range) && !exe[i].diff.intersects(range);
                }
                else if (exe[i].zeroFilled(rva, r.size)) {
                    // uninitialized data: there are no bytes to compare; accept it when code that is
                    // byte-identical in the file reads or writes it
                    for (Address x = a; !r.ok[i] && x.compareTo(a.add(r.size - 1)) <= 0; x = x.add(1)) {
                        for (Reference ref : p.getReferenceManager().getReferencesTo(x)) {
                            Function g = fm.getFunctionContaining(ref.getFromAddress());
                            if (g != null && bytesMatch(g.getBody(), exe[i])) { r.ok[i] = true; break; }
                        }
                    }
                }
            }
            data.add(r);
        }
        data.sort(Comparator.comparing((DataRow r) -> r.addr).thenComparing(r -> r.name));

        // ---- types ----
        Set<DataType> needed = new LinkedHashSet<>();
        Set<DataType> seen = new HashSet<>();
        DataTypeManager dtm = p.getDataTypeManager();
        List<DataType> candidates = new ArrayList<>();
        for (Iterator<DataType> it = dtm.getAllDataTypes(); it.hasNext();) {
            DataType d = it.next();
            if (!(d instanceof Composite || d instanceof Enum || d instanceof TypeDef)) continue;
            if (!programOwned(d) || generated(d)) continue;
            if (d.getName().endsWith(".conflict")) { skip("type: leftover .conflict copy"); continue; }
            if (d instanceof Structure && ((Structure) d).isNotYetDefined()) continue;   // placeholders come in only when used
            candidates.add(d);
        }
        candidates.sort(Comparator.comparing((DataType d) -> d.getCategoryPath().getPath()).thenComparing(DataType::getName));
        for (DataType d : candidates) needType(d, needed, seen);
        for (FnRow r : rows) {
            if (r.sig.isEmpty()) continue;
            needType(r.f.getReturnType(), needed, seen);
            for (Parameter prm : r.f.getParameters()) needType(prm.getDataType(), needed, seen);
        }
        for (DataRow r : data) {
            Data d = listing.getDataAt(r.addr);
            if (d != null) needType(d.getDataType(), needed, seen);
        }

        String typesText = writeTypes(needed);
        // signatures and data types must use the same C names as types.h
        for (Map.Entry<String,String> e : renamedTypes.entrySet()) {
            Pattern oldName = Pattern.compile("(?<![A-Za-z0-9_])" + Pattern.quote(e.getKey()) + "(?![A-Za-z0-9_])");
            String nu = Matcher.quoteReplacement(e.getValue());
            for (FnRow r : rows) if (!r.sig.isEmpty()) r.sig = oldName.matcher(r.sig).replaceAll(nu);
            for (DataRow r : data) r.type = oldName.matcher(r.type).replaceAll(nu);
        }

        // ---- write ----
        StringBuilder fn = new StringBuilder();
        fn.append("address\tsize\tname\tsignature\tsource\tconfidence\tverified_bos\tverified_bos_hr\n");
        StringBuilder un = new StringBuilder();
        un.append("address\tsize\tname\tsource\tconfidence\tbytes_differing_from_bos\tbytes_differing_from_bos_hr\n");
        int nOk = 0, nUn = 0;
        Map<String,Integer> bySource = new TreeMap<>(), byConf = new TreeMap<>();
        int onlyBos = 0, onlyHr = 0, both = 0;
        for (FnRow r : rows) {
            if (!r.ok[0] && !r.ok[1]) {
                un.append(String.format("%08x\t%d\t%s\t%s\t%s\t%d\t%d\n", r.addr.getOffset(), r.size, r.name, r.source, r.conf, r.badBytes[0], r.badBytes[1]));
                nUn++;
                continue;
            }
            fn.append(String.format("%08x\t%d\t%s\t%s\t%s\t%s\t%s\t%s\n", r.addr.getOffset(), r.size, r.name, r.sig, r.source, r.conf, r.ok[0] ? "yes" : "no", r.ok[1] ? "yes" : "no"));
            nOk++;
            bySource.merge(r.source, 1, Integer::sum);
            if (r.source.equals("agent")) byConf.merge(r.conf, 1, Integer::sum);
            if (r.ok[0] && r.ok[1]) both++; else if (r.ok[0]) onlyBos++; else onlyHr++;
        }
        StringBuilder dt = new StringBuilder();
        dt.append("address\tsize\tname\ttype\tsource\tverified_bos\tverified_bos_hr\n");
        int dBoth = 0;
        for (DataRow r : data) {
            dt.append(String.format("%08x\t%d\t%s\t%s\t%s\t%s\t%s\n", r.addr.getOffset(), r.size, r.name, r.type, r.source, r.ok[0] ? "yes" : "no", r.ok[1] ? "yes" : "no"));
            if (r.ok[0] && r.ok[1]) dBoth++;
        }
        write(outDir.resolve("functions.tsv"), fn.toString());
        write(outDir.resolve("functions-unverified.tsv"), un.toString());
        write(outDir.resolve("data.tsv"), dt.toString());
        write(outDir.resolve("types.h"), typesText);
        write(outDir.resolve("verification.txt"), verificationReport(exe, rows, nOk, nUn, both, onlyBos, onlyHr, data.size(), dBoth));

        println("RESULT functions.tsv rows=" + nOk + " (verified for both=" + both + ", only BOS.exe=" + onlyBos + ", only BOS_HR.exe=" + onlyHr + ")");
        println("RESULT functions-unverified.tsv rows=" + nUn);
        println("RESULT by source " + bySource + " agent confidence " + byConf);
        println("RESULT data.tsv rows=" + data.size() + " (verified for both=" + dBoth + ")");
        println("RESULT types.h types=" + needed.size());
        println("RESULT not exported: " + skipped);
    }

    void write(Path path, String text) throws IOException {
        Files.write(path, text.getBytes(StandardCharsets.UTF_8));
    }

    // ---- types.h -----------------------------------------------------------------------------
    String writeTypes(Set<DataType> needed) throws Exception {
        DataTypeManager src = currentProgram.getDataTypeManager();
        // Work on a copy so names can be made valid C and descriptions dropped without touching the program.
        StandAloneDataTypeManager tmp = new StandAloneDataTypeManager("fotkit", src.getDataOrganization());
        List<DataType> copies = new ArrayList<>();
        Map<String,String> renamed = renamedTypes;
        int tx = tmp.startTransaction("copy");
        try {
            Map<DataType,DataType> copyOf = new LinkedHashMap<>();
            for (DataType d : needed) {
                DataType c = tmp.resolve(d, DataTypeConflictHandler.REPLACE_HANDLER);
                copyOf.put(d, c);
            }
            // names that are not C identifiers
            Set<String> used = new HashSet<>();
            for (DataType c : copyOf.values()) used.add(c.getName());
            for (DataType c : new ArrayList<>(copyOf.values())) {
                String old = c.getName();
                String nu = cIdent(old);
                if (!nu.equals(old)) {
                    while (used.contains(nu)) nu = nu + "_";
                    c.setName(nu);
                    used.add(nu);
                    renamed.put(old, nu);
                }
            }
            for (DataType c : copyOf.values()) {
                if (c instanceof Composite) {
                    Composite comp = (Composite) c;
                    comp.setDescription("");
                    for (DataTypeComponent dc : comp.getComponents()) dc.setComment(null);
                }
                else if (c instanceof Enum) {
                    ((Enum) c).setDescription("");
                }
                copies.add(c);
            }
        }
        finally {
            tmp.endTransaction(tx, true);
        }
        // one definition per C name: if two types share a name, keep the defined one
        Map<String,DataType> byName = new TreeMap<>();
        List<DataType> unique = new ArrayList<>();
        for (DataType c : copies) {
            DataType prev = byName.get(c.getName());
            if (prev == null) { byName.put(c.getName(), c); unique.add(c); continue; }
            println("WARN type name used twice, keeping the first: " + c.getName());
        }
        unique.sort(Comparator.comparing(DataType::getName));

        StringWriter sw = new StringWriter();
        DataTypeWriter w = new DataTypeWriter(tmp, sw);
        int preamble = sw.getBuffer().length();     // typedefs for Ghidra's built-in names: the C parser already knows those
        w.write(unique, TaskMonitor.DUMMY, false);
        String text = sw.toString().substring(preamble).replace("\r\n", "\n");
        // "typedef struct X X, *PX;" -> "typedef struct X X;" (the pointer typedefs would only clutter a database)
        text = text.replaceAll("(?m)^(typedef (?:struct|union) (\\S+) \\2), \\*P\\S+;$", "$1;");
        text = text.replaceAll("(?s)/\\*.*?\\*/", "");   // belt and braces: no comments at all
        text = collapseFiller(text);
        StringBuilder sb = new StringBuilder();
        sb.append("/* Type layouts for Fallout Tactics: Brotherhood of Steel 1.27 (BOS.exe, BOS_HR.exe).\n");
        sb.append(" * Generated by ghidra/ExportFotSymbols.java in the style of Ghidra's \"Export C header\".\n");
        sb.append(" * Names and layouts only. Built-in Ghidra type names (undefined4, byte, dword, pointer...) are\n");
        sb.append(" * used as they are. Names that were not valid C (such as Actor::ClassType) use __ instead of ::.\n");
        sb.append(" * Fields are packed exactly as listed (pack 1); gaps are written out as undefined bytes.\n");
        sb.append(" * Import it with ghidra/ApplyFotSymbols.java, or with File > Parse C Source.\n");
        sb.append(" */\n");
        sb.append("#pragma pack(push, 1)\n\n");
        sb.append(text.trim()).append("\n\n");
        sb.append("#pragma pack(pop)\n");
        String header = sb.toString();
        for (Map.Entry<String,String> e : renamed.entrySet()) println("RENAMED type " + e.getKey() + " -> " + e.getValue());
        int packed = 0;
        for (DataType c : unique) if (c instanceof Structure && ((Structure) c).isPackingEnabled()) packed++;
        println("TYPES written=" + unique.size() + " (structs with packing enabled in the database: " + packed + ")");
        roundTrip(header, unique);
        return header;
    }

    // Runs of single "undefined" filler fields become one array, so a 40-byte hole is one line, not forty.
    static String collapseFiller(String text) {
        Pattern one = Pattern.compile("^    undefined (field(\\d+)_0x([0-9a-f]+));$");
        String[] lines = text.split("\n", -1);
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < lines.length; i++) {
            Matcher m = one.matcher(lines[i]);
            if (!m.matches()) { out.append(lines[i]); if (i < lines.length - 1) out.append('\n'); continue; }
            long start = Long.parseLong(m.group(3), 16);
            int n = 1;
            while (i + 1 < lines.length) {
                Matcher nx = one.matcher(lines[i + 1]);
                if (!nx.matches() || Long.parseLong(nx.group(3), 16) != start + n) break;
                n++;
                i++;
            }
            out.append("    undefined ").append(m.group(1));
            if (n > 1) out.append('[').append(n).append(']');
            out.append(';');
            if (i < lines.length - 1) out.append('\n');
        }
        return out.toString();
    }

    // Parses the generated header with Ghidra's own C parser and checks every struct against the original.
    void roundTrip(String header, List<DataType> copies) throws Exception {
        Path tmpFile = Files.createTempFile(ghidra.framework.Application.getUserTempDirectory().toPath(), "fotkit-types-", ".h");
        StandAloneDataTypeManager parsed = new StandAloneDataTypeManager("parsed", currentProgram.getDataTypeManager().getDataOrganization());
        int tx = parsed.startTransaction("parse");
        ghidra.app.util.cparser.C.CParserUtils.CParseResults res;
        try {
            Files.write(tmpFile, header.getBytes(StandardCharsets.UTF_8));
            res = ghidra.app.util.cparser.C.CParserUtils.parseHeaderFiles(new DataTypeManager[0], new String[] { tmpFile.toString() }, new String[0], new String[0], parsed, monitor);
        }
        finally {
            parsed.endTransaction(tx, true);
            Files.deleteIfExists(tmpFile);
        }
        if (!res.successful()) println("ROUNDTRIP WARNING: the C parser reported problems: " + res.getFormattedParseMessage(null));
        int structs = 0, same = 0, missing = 0;
        for (DataType c : copies) {
            if (!(c instanceof Structure)) continue;
            structs++;
            List<DataType> found = new ArrayList<>();
            parsed.findDataTypes(c.getName(), found);
            DataType q = null;
            for (DataType f : found) if (f instanceof Structure) q = f;
            if (q == null) { missing++; println("ROUNDTRIP missing after parse: " + c.getName()); continue; }
            String why = layoutDifference((Structure) c, (Structure) q);
            if (why == null) same++;
            else println("ROUNDTRIP differs: " + c.getName() + " - " + why);
        }
        println("ROUNDTRIP structs=" + structs + " identical layout=" + same + " missing=" + missing + " parser ok=" + res.successful());
    }

    static boolean filler(DataTypeComponent dc) {
        DataType d = stripToBase(dc.getDataType());
        return d == null || d == DataType.DEFAULT || d instanceof Undefined;
    }

    static String layoutDifference(Structure a, Structure b) {
        if (a.getLength() != b.getLength()) return "length " + a.getLength() + " vs " + b.getLength();
        List<DataTypeComponent> ca = new ArrayList<>(), cb = new ArrayList<>();
        for (DataTypeComponent dc : a.getDefinedComponents()) if (!filler(dc)) ca.add(dc);
        for (DataTypeComponent dc : b.getDefinedComponents()) if (!filler(dc)) cb.add(dc);
        if (ca.size() != cb.size()) return "field count " + ca.size() + " vs " + cb.size();
        for (int i = 0; i < ca.size(); i++) {
            DataTypeComponent x = ca.get(i), y = cb.get(i);
            if (x.getOffset() != y.getOffset() || x.getLength() != y.getLength()) return "field " + x.getFieldName() + " at " + x.getOffset() + "/" + x.getLength() + " vs " + y.getOffset() + "/" + y.getLength();
            String xn = x.getFieldName(), yn = y.getFieldName();
            if (xn != null && !xn.equals(yn)) return "field name " + xn + " vs " + yn;
        }
        return null;
    }

    String verificationReport(Exe[] exe, List<FnRow> rows, int nOk, int nUn, int both, int onlyBos, int onlyHr, int nData, int dBoth) {
        StringBuilder sb = new StringBuilder();
        sb.append("How the symbol files were checked against the executables\n");
        sb.append("==========================================================\n\n");
        sb.append("Every named function was compared byte for byte with the same address in each executable;\n");
        sb.append("the file offset comes from the executable's PE section table. The program and both\n");
        sb.append("executables are linked at 0x").append(Long.toHexString(programBase)).append(" with the same sections, so no relocation masking is needed.\n");
        sb.append("Addresses below are addresses in the program, not file offsets.\n\n");
        for (Exe x : exe) {
            sb.append(x.name).append("  SHA-256 ").append(x.sha).append("\n");
            sb.append("  bytes of the program that differ from this file: ").append(x.diffBytes).append(" in ").append(x.diff.getNumAddressRanges()).append(" ranges\n");
            for (AddressRange r : x.diff) {
                MemoryBlock b = currentProgram.getMemory().getBlock(r.getMinAddress());
                sb.append(String.format("    %08x  %d byte%s  %s%n", r.getMinAddress().getOffset(), r.getLength(), r.getLength() == 1 ? "" : "s", b == null ? "?" : b.getName()));
            }
            sb.append("\n");
        }
        sb.append("Named functions considered: ").append(rows.size()).append("\n");
        sb.append("  verified for both executables:   ").append(both).append("\n");
        sb.append("  verified for BOS.exe only:       ").append(onlyBos).append("\n");
        sb.append("  verified for BOS_HR.exe only:    ").append(onlyHr).append("\n");
        sb.append("  verified for neither (listed in functions-unverified.tsv): ").append(nUn).append("\n\n");
        sb.append("Named data symbols: ").append(nData).append(", verified for both executables: ").append(dBoth).append("\n");
        return sb.toString();
    }
}
