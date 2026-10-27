// MIT licensed (see ../LICENSE).
//
// Applies the Fallout Tactics symbol kit to your own copy of BOS.exe or BOS_HR.exe:
// it imports the type layouts, then names functions and global data and sets
// function signatures. Nothing of the game is stored in the kit, only names,
// addresses, sizes, signatures and type layouts.
//
// BEFORE YOU RUN IT
//   1. Import BOS.exe 1.27 (Steam or GOG) or Steam's BOS_HR.exe into a Ghidra 12.x project.
//      Use the default x86 32-bit Windows (PE) settings and leave the image base at 0x400000.
//   2. Run auto-analysis with the default options.
//   3. Run this script (Script Manager, or headless; see the README in the kit).
//
// WHAT IT DOES
//   - Checks the program's SHA-256 first and stops if it is neither of
//       BOS.exe     c5b91fe56ef8de118ce22ccd5e6512f217ca16c8b8758ea14c626229eb5a4e40
//       BOS_HR.exe  ae7ddade7f3c5c48552b54f72b795b3919477b789bba7c910db46c8e75c812d0
//   - Applies only the rows that symbols/*.tsv mark as verified for that executable.
//   - Types: parses symbols/types.h and adds the structs and typedefs under /FOT.
//     A type that already exists is kept; an empty placeholder is completed.
//   - Functions: where analysis found no function at an address of a verified row,
//     the script disassembles there and creates one. Then it sets the name and
//     namespace and, where the kit has one that parses, the signature. Names keep
//     the kind of source the kit records: ftse and community names are USER_DEFINED,
//     deterministic names ANALYSIS and agent names IMPORTED.
//   - Agent (AI-made) names are guesses. Each one gets a plate comment line
//     [agent conf=high|medium|low] (the only comment this script writes), so a guess
//     is recognisable in the listing. Low-confidence names are only hints.
//   - Data: adds labels and, where the type is more than "undefined", data types.
//   - Prints counts, and why anything was not applied.
//
// SAFE TO RUN AGAIN
//   Everything it applied is recorded in a program property map ("FotSymbols").
//   A second run finds those entries unchanged and does nothing. A name you changed
//   since is left alone (unless you pass overwrite=yes); a name the kit changed
//   since is updated. Placeholder names Ghidra's analysis made (Unwind@..., Catch@...)
//   are replaced. A C-library name that analysis identified is kept in preference to
//   an AI-made name.
//
// OPTIONS (script arguments, all optional)
//   dir=<folder>        the kit's symbols folder (default: ../symbols next to this script)
//   minconf=<level>     skip agent names below low|medium|high (default low = apply all)
//   tags=no             do not write the [agent conf=...] plate comment lines
//   overwrite=yes       replace names and signatures you changed yourself
//   types=no | functions=no | data=no     skip a part
//
// To refresh the kit after you improve names, see ExportFotSymbols.java.
//
//@category FOT

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.cmd.function.FunctionRenameOption;
import ghidra.app.script.GhidraScript;
import ghidra.app.util.cparser.C.CParserUtils;
import ghidra.app.util.parser.FunctionSignatureParser;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.data.Enum;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.util.*;
import ghidra.util.data.DataTypeParser;
import ghidra.util.exception.CancelledException;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.regex.*;

public class ApplyFotSymbols extends GhidraScript {

    static final String[] EXE_NAME = { "BOS.exe", "BOS_HR.exe" };
    static final String[] EXE_SHA256 = {
        "c5b91fe56ef8de118ce22ccd5e6512f217ca16c8b8758ea14c626229eb5a4e40",
        "ae7ddade7f3c5c48552b54f72b795b3919477b789bba7c910db46c8e75c812d0" };
    static final long IMAGE_BASE = 0x400000L;
    static final String PROPERTY_MAP = "FotSymbols";
    static final CategoryPath KIT_CATEGORY = new CategoryPath("/FOT");
    static final CategoryPath KIT_FUNCTIONS = new CategoryPath("/FOT/functions");
    static final Pattern AGENT_TAG = Pattern.compile("\\[agent conf=[^\\]\\r\\n]*\\]");

    // ---- options and state ------------------------------------------------------------------
    int target;                       // 0 = BOS.exe, 1 = BOS_HR.exe
    int minConf = 0;                  // 0 low, 1 medium, 2 high
    boolean writeTags = true, overwrite = false, doTypes = true, doFunctions = true, doData = true;
    File symbolsDir;

    final Map<String,Integer> counts = new LinkedHashMap<>();
    final Map<String,List<String>> examples = new LinkedHashMap<>();
    StringPropertyMap applied;
    SymbolTable symbols;
    FunctionManager functions;
    Listing listing;
    final Map<String,Namespace> namespaceCache = new HashMap<>();

    void count(String key) { counts.merge(key, 1, Integer::sum); }

    void fail(String key, String detail) {
        count(key);
        List<String> l = examples.computeIfAbsent(key, k -> new ArrayList<>());
        if (l.size() < 4) l.add(detail);
    }

    // ---- main -------------------------------------------------------------------------------
    @Override
    public void run() throws Exception {
        if (currentProgram == null) { printerr("Open a program first."); return; }
        if (!identifyExecutable()) return;
        parseArguments();
        if (!locateSymbols()) return;

        symbols = currentProgram.getSymbolTable();
        functions = currentProgram.getFunctionManager();
        listing = currentProgram.getListing();
        PropertyMapManager pmm = currentProgram.getUsrPropertyManager();
        applied = pmm.getStringPropertyMap(PROPERTY_MAP);
        if (applied == null) applied = pmm.createStringPropertyMap(PROPERTY_MAP);

        println("FOT symbol kit: target " + EXE_NAME[target] + ", symbols from " + symbolsDir);
        try {
            if (doTypes) importTypes(new File(symbolsDir, "types.h"));
            if (doFunctions) applyFunctions(new File(symbolsDir, "functions.tsv"));
            if (doData) applyData(new File(symbolsDir, "data.tsv"));
        }
        catch (IOException e) {
            printerr("Cannot read the kit's files: " + e.getMessage());
        }
        printSummary();
    }

    // ---- which executable is this ------------------------------------------------------------
    boolean identifyExecutable() throws Exception {
        String sha = currentProgram.getExecutableSHA256();
        if (sha == null || sha.isEmpty()) {
            // programs imported by other means may lack the hash: try the file it was imported from
            String path = currentProgram.getExecutablePath();
            if (path != null) {
                if (path.matches("^/[A-Za-z]:.*")) path = path.substring(1);
                File f = new File(path);
                if (f.isFile()) {
                    MessageDigest md = MessageDigest.getInstance("SHA-256");
                    StringBuilder sb = new StringBuilder();
                    for (byte b : md.digest(Files.readAllBytes(f.toPath()))) sb.append(String.format("%02x", b & 0xff));
                    sha = sb.toString();
                }
            }
        }
        sha = sha == null ? "" : sha.toLowerCase(Locale.ROOT);
        target = -1;
        for (int i = 0; i < EXE_SHA256.length; i++) if (EXE_SHA256[i].equals(sha)) target = i;
        if (target < 0) {
            String msg = "Refusing to run: this program's SHA-256 is " + (sha.isEmpty() ? "unknown" : sha) + ".\n" +
                "The kit supports only BOS.exe 1.27 (" + EXE_SHA256[0] + ")\n" +
                "and BOS_HR.exe (" + EXE_SHA256[1] + ").\n" +
                "Import one of those from Steam or GOG, unpatched, and run this script on that program.";
            printerr(msg);
            if (!isRunningHeadless()) popup(msg);
            return false;
        }
        if (currentProgram.getImageBase().getOffset() != IMAGE_BASE) {
            String msg = "Refusing to run: the image base is " + currentProgram.getImageBase() + ", the kit's addresses assume 0x400000.";
            printerr(msg);
            if (!isRunningHeadless()) popup(msg);
            return false;
        }
        return true;
    }

    void parseArguments() {
        for (String a : getScriptArgs()) {
            int eq = a.indexOf('=');
            String k = (eq < 0 ? a : a.substring(0, eq)).trim().toLowerCase(Locale.ROOT);
            String v = eq < 0 ? "yes" : a.substring(eq + 1).trim();
            boolean yes = !(v.equalsIgnoreCase("no") || v.equalsIgnoreCase("false") || v.equals("0"));
            switch (k) {
                case "dir": symbolsDir = new File(v); break;
                case "minconf":
                    minConf = v.equalsIgnoreCase("high") ? 2 : v.equalsIgnoreCase("medium") ? 1 : 0;
                    break;
                case "tags": writeTags = yes; break;
                case "overwrite": overwrite = yes; break;
                case "types": doTypes = yes; break;
                case "functions": doFunctions = yes; break;
                case "data": doData = yes; break;
                default: printerr("Ignoring unknown argument: " + a);
            }
        }
    }

    boolean locateSymbols() throws Exception {
        if (symbolsDir == null && getSourceFile() != null) {
            File script = getSourceFile().getFile(false);
            if (script != null && script.getParentFile() != null && script.getParentFile().getParentFile() != null) {
                symbolsDir = new File(script.getParentFile().getParentFile(), "symbols");
            }
        }
        if ((symbolsDir == null || !new File(symbolsDir, "functions.tsv").isFile()) && !isRunningHeadless()) {
            symbolsDir = askDirectory("Select the kit's symbols folder", "Use");
        }
        if (symbolsDir == null || !new File(symbolsDir, "functions.tsv").isFile()) {
            printerr("Cannot find symbols/functions.tsv. Pass dir=<path to the kit's symbols folder>.");
            return false;
        }
        return true;
    }

    // ---- tab-separated files -----------------------------------------------------------------
    static class Table {
        Map<String,Integer> col = new HashMap<>();
        List<String[]> rows = new ArrayList<>();
        String get(String[] r, String c) { Integer i = col.get(c); return i == null || i >= r.length ? "" : r[i]; }
    }

    Table readTable(File f, String... required) throws IOException {
        Table t = new Table();
        List<String> lines = Files.readAllLines(f.toPath(), StandardCharsets.UTF_8);
        if (lines.isEmpty()) throw new IOException(f + " is empty");
        String[] head = lines.get(0).split("\t", -1);
        for (int i = 0; i < head.length; i++) t.col.put(head[i].trim(), i);
        for (String c : required) if (!t.col.containsKey(c)) throw new IOException(f + " has no column " + c);
        for (int i = 1; i < lines.size(); i++) {
            if (lines.get(i).isEmpty()) continue;
            t.rows.add(lines.get(i).split("\t", -1));
        }
        return t;
    }

    // ---- types ------------------------------------------------------------------------------
    static boolean placeholder(DataType d) {
        if (d instanceof Structure) {
            Structure s = (Structure) d;
            return s.isNotYetDefined() || s.getNumComponents() == 0;
        }
        if (d instanceof Union) return ((Union) d).getNumComponents() == 0;
        return false;
    }

    static DataType stripToBase(DataType d) {
        while (d != null) {
            if (d instanceof Pointer) d = ((Pointer) d).getDataType();
            else if (d instanceof Array) d = ((Array) d).getDataType();
            else break;
        }
        return d;
    }

    // keeps what is already in the program; completes an empty placeholder with the real layout
    static final DataTypeConflictHandler KEEP_BUT_COMPLETE = new DataTypeConflictHandler() {
        @Override
        public ConflictResult resolveConflict(DataType added, DataType existing) {
            if (placeholder(existing) && !placeholder(added)) return ConflictResult.REPLACE_EXISTING;
            return ConflictResult.USE_EXISTING;
        }
        @Override
        public boolean shouldUpdate(DataType source, DataType local) { return false; }
        @Override
        public DataTypeConflictHandler getSubsequentHandler() { return this; }
    };

    DataType findExisting(DataTypeManager dtm, String name) {
        List<DataType> found = new ArrayList<>();
        dtm.findDataTypes(name, found);
        DataType best = null;
        for (DataType d : found) {
            if (!(d instanceof Composite || d instanceof Enum || d instanceof TypeDef)) continue;
            if (d.getCategoryPath().equals(KIT_CATEGORY)) return d;
            if (best == null) best = d;
        }
        return best;
    }

    void importTypes(File header) throws Exception {
        if (!header.isFile()) { fail("types: types.h not found", header.toString()); return; }
        DataTypeManager dtm = currentProgram.getDataTypeManager();
        StandAloneDataTypeManager tmp = new StandAloneDataTypeManager("fotkit", dtm.getDataOrganization());
        try {
            CParserUtils.CParseResults res;
            int tx = tmp.startTransaction("parse");
            try {
                res = CParserUtils.parseHeaderFiles(new DataTypeManager[0], new String[] { header.getAbsolutePath() }, new String[0], new String[0], tmp, monitor);
            }
            finally {
                tmp.endTransaction(tx, true);
            }
            if (!res.successful()) {
                fail("types: the C parser rejected types.h", res.getFormattedParseMessage(null));
                return;
            }

            List<DataType> mine = new ArrayList<>();
            tx = tmp.startTransaction("prepare");
            try {
                nameFunctionPointerTypes(tmp);
                for (Iterator<DataType> it = tmp.getAllDataTypes(); it.hasNext();) {
                    DataType d = it.next();
                    if (d instanceof Composite || d instanceof Enum || d instanceof TypeDef) mine.add(d);
                }
                mine.sort(Comparator.comparing(DataType::getName));
                // an existing type of the same name decides the category, so the two meet in the program
                for (DataType t : mine) {
                    DataType ex = findExisting(dtm, t.getName());
                    t.setCategoryPath(ex != null ? ex.getCategoryPath() : KIT_CATEGORY);
                }
            }
            finally {
                tmp.endTransaction(tx, true);
            }

            Map<DataType,DataType> before = new HashMap<>();
            for (DataType t : mine) before.put(t, dtm.getDataType(t.getCategoryPath(), t.getName()));
            for (DataType t : mine) {
                monitor.checkCancelled();
                try {
                    dtm.resolve(t, KEEP_BUT_COMPLETE);
                }
                catch (Exception e) {
                    fail("types: could not add", t.getName() + ": " + e.getMessage());
                }
            }
            for (DataType t : mine) {
                DataType was = before.get(t);
                DataType now = dtm.getDataType(t.getCategoryPath(), t.getName());
                if (now == null) continue;
                if (was == null) count("types created");
                else if (placeholder(was) && !placeholder(now)) count("types completed (placeholder replaced)");
                else count("types already present");
            }
        }
        finally {
            tmp.close();
        }
    }

    // C function-pointer fields arrive as anonymous "_func_N" definitions; name them after their field
    void nameFunctionPointerTypes(DataTypeManager tmp) throws Exception {
        Set<String> used = new HashSet<>();
        List<Structure> structs = new ArrayList<>();
        for (Iterator<DataType> it = tmp.getAllDataTypes(); it.hasNext();) {
            DataType d = it.next();
            if (d instanceof FunctionDefinition) used.add(d.getName());
            if (d instanceof Structure) structs.add((Structure) d);
        }
        structs.sort(Comparator.comparing(DataType::getName));
        for (Structure s : structs) {
            for (DataTypeComponent dc : s.getDefinedComponents()) {
                DataType b = stripToBase(dc.getDataType());
                if (!(b instanceof FunctionDefinition) || !b.getName().startsWith("_func_")) continue;
                String field = dc.getFieldName() != null ? dc.getFieldName() : "field" + dc.getOrdinal();
                String base = s.getName() + "_" + field;
                String nm = base;
                for (int n = 2; !used.add(nm); n++) nm = base + "_" + n;
                b.setName(nm);
                b.setCategoryPath(KIT_FUNCTIONS);
            }
        }
    }

    // ---- names -------------------------------------------------------------------------------
    // "A::B<C::D>::name" -> [A, B<C::D>, name]; "::" inside angle brackets does not split
    static List<String> splitName(String s) {
        List<String> out = new ArrayList<>();
        int depth = 0, start = 0;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '<') depth++;
            else if (c == '>') { if (depth > 0) depth--; }
            else if (c == ':' && depth == 0 && i + 1 < s.length() && s.charAt(i + 1) == ':' && i > start) {
                out.add(s.substring(start, i));
                start = i + 2;
                i++;
            }
        }
        out.add(s.substring(start));
        return out;
    }

    Namespace namespaceFor(List<String> path) throws Exception {
        Namespace cur = currentProgram.getGlobalNamespace();
        StringBuilder key = new StringBuilder();
        for (String seg : path) {
            key.append("::").append(seg);
            Namespace next = namespaceCache.get(key.toString());
            if (next == null) {
                next = symbols.getNamespace(seg, cur);
                if (next == null) {
                    next = symbols.createClass(cur, seg, SourceType.USER_DEFINED);
                    count("namespaces created");
                }
                namespaceCache.put(key.toString(), next);
            }
            cur = next;
        }
        return cur;
    }

    static SourceType sourceTypeFor(String source) {
        switch (source) {
            case "ftse":
            case "community": return SourceType.USER_DEFINED;
            case "deterministic": return SourceType.ANALYSIS;
            default: return SourceType.IMPORTED;
        }
    }

    static int confidenceLevel(String c) {
        return c.equals("high") ? 2 : c.equals("medium") ? 1 : 0;
    }

    // ---- functions ---------------------------------------------------------------------------
    void applyFunctions(File file) throws Exception {
        Table t = readTable(file, "address", "size", "name", "signature", "source", "confidence", "verified_bos", "verified_bos_hr");
        String verifiedCol = target == 0 ? "verified_bos" : "verified_bos_hr";
        println("FUNCTIONS: " + t.rows.size() + " rows in the kit");
        int done = 0;
        for (String[] r : t.rows) {
            monitor.checkCancelled();
            if (!t.get(r, verifiedCol).equals("yes")) { count("functions not verified for " + EXE_NAME[target]); continue; }
            String source = t.get(r, "source");
            if (source.equals("agent") && confidenceLevel(t.get(r, "confidence")) < minConf) { count("functions skipped (agent confidence below minconf)"); continue; }
            try {
                applyFunction(t, r);
            }
            catch (CancelledException e) {
                throw e;
            }
            catch (Exception e) {
                fail("functions: unexpected error", t.get(r, "address") + " " + t.get(r, "name") + ": " + e);
            }
            if (++done % 2000 == 0) println("  ..." + done + " rows");
        }
    }

    void applyFunction(Table t, String[] r) throws Exception {
        Address addr = toAddr(Long.parseLong(t.get(r, "address"), 16));
        String name = t.get(r, "name"), sig = t.get(r, "signature"), source = t.get(r, "source");
        String conf = t.get(r, "confidence");
        String stamp = name + "\t" + sig + "\t" + source + "\t" + conf;

        Function f = functions.getFunctionAt(addr);
        if (f == null) {
            f = createFunctionAt(addr, name);
            if (f == null) return;
            count("functions created where analysis found none");
        }
        String recorded = applied.getString(addr);
        String current = f.getName(true);
        boolean sameName = current.equals(name);

        // a previous run with the same row and nothing changed since: nothing to do
        if (recorded != null && recorded.equals(stamp) && sameName) { count("functions already applied"); return; }

        // decide whether the current name may be replaced
        SourceType cs = f.getSymbol().getSource();
        boolean mayRename;
        if (sameName) mayRename = true;
        else if (overwrite) mayRename = true;
        else if (cs == SourceType.DEFAULT) mayRename = true;
        else if (recorded != null && recorded.startsWith(current + "\t")) mayRename = true;      // the kit's own earlier name, untouched since
        else if (cs == SourceType.ANALYSIS) mayRename = isFuncletName(f.getName()) || !source.equals("agent");
        else mayRename = false;

        boolean renamed = false;
        if (!sameName) {
            if (mayRename) {
                renamed = setName(f, name, source, addr);
            }
            else {
                count("function names kept (already named " + (cs == SourceType.USER_DEFINED ? "by you" : "by analysis or import") + ")");
            }
        }
        else {
            count("functions already named as in the kit");
        }
        if (renamed) count("function names applied");

        boolean nameOk = f.getName(true).equals(name);
        if (nameOk && writeTags && source.equals("agent")) setTag(f, conf);

        // signature
        boolean sigApplied = false;
        if (!sig.isEmpty() && nameOk) {
            boolean userSig = f.getSignatureSource() == SourceType.USER_DEFINED;
            String recSig = recorded == null ? null : recorded.split("\t", -1)[1];
            boolean mineAlready = recSig != null && recSig.equals(sig) && recorded.startsWith(name + "\t");
            if (mineAlready) count("signatures already applied");
            else if (userSig && !overwrite && recSig == null && !samePrototype(f, sig)) count("signatures kept (you already set one)");
            else sigApplied = applySignature(f, sig);
        }

        // remember what the kit did here, for the next run
        if (nameOk && (sig.isEmpty() || sigApplied || (recorded != null && recorded.equals(stamp)))) applied.add(addr, stamp);
        else if (nameOk && !sig.isEmpty()) applied.add(addr, name + "\t\t" + source + "\t" + conf);

        long kitSize = -1;
        try { kitSize = Long.parseLong(t.get(r, "size")); } catch (NumberFormatException e) { /* a row without a size is fine */ }
        if (kitSize >= 0 && f.getBody().getNumAddresses() != kitSize) count("info: function body differs in size from the kit's (bytes are verified, so names still apply)");
    }

    static boolean isFuncletName(String n) {
        return n.startsWith("Unwind@") || n.startsWith("Catch@");
    }

    boolean samePrototype(Function f, String sig) {
        return f.getPrototypeString(false, true).equals(sig);
    }

    Function createFunctionAt(Address addr, String name) {
        Function inside = functions.getFunctionContaining(addr);
        if (inside != null) { fail("functions: address is inside another function", addr + " " + name + " (inside " + inside.getName(true) + ")"); return null; }
        if (listing.getInstructionAt(addr) == null) {
            Data d = listing.getDefinedDataAt(addr);
            if (d != null) { fail("functions: defined data at the address (left alone)", addr + " " + name + " (" + d.getDataType().getName() + ")"); return null; }
            DisassembleCommand dis = new DisassembleCommand(addr, null, true);
            if (!dis.applyTo(currentProgram, monitor) || listing.getInstructionAt(addr) == null) {
                fail("functions: could not disassemble", addr + " " + name + " " + dis.getStatusMsg());
                return null;
            }
        }
        CreateFunctionCmd cmd = new CreateFunctionCmd(addr);
        if (!cmd.applyTo(currentProgram, monitor)) {
            fail("functions: could not create a function", addr + " " + name + " " + cmd.getStatusMsg());
            return null;
        }
        Function f = functions.getFunctionAt(addr);
        if (f == null) fail("functions: could not create a function", addr + " " + name);
        return f;
    }

    boolean setName(Function f, String name, String source, Address addr) {
        List<String> parts = splitName(name);
        String simple = parts.get(parts.size() - 1);
        try {
            Namespace ns = namespaceFor(parts.subList(0, parts.size() - 1));
            f.getSymbol().setNameAndNamespace(simple, ns, sourceTypeFor(source));
            return true;
        }
        catch (Exception e) {
            fail("functions: Ghidra rejected the name", addr + " " + name + ": " + e.getMessage());
            return false;
        }
    }

    void setTag(Function f, String conf) {
        String tag = "[agent conf=" + conf + "]";
        String cur = f.getComment();
        String nu;
        if (cur == null || cur.isEmpty()) nu = tag;
        else if (AGENT_TAG.matcher(cur).find()) nu = AGENT_TAG.matcher(cur).replaceFirst(Matcher.quoteReplacement(tag));
        else nu = tag + "\n" + cur;
        if (!nu.equals(cur)) { f.setComment(nu); count("agent confidence tags written"); }
    }

    static final Pattern CALLING_CONVENTION = Pattern.compile("\\b__(?:cdecl|stdcall|fastcall|thiscall)\\b");

    // Ghidra's two signature parsers do not read a calling convention inside the text, so it is split off and set on the result.
    FunctionDefinitionDataType parseSignature(String sig, String[] why) {
        String cc = null, text = sig;
        Matcher m = CALLING_CONVENTION.matcher(sig);
        if (m.find()) {
            cc = m.group();
            text = (sig.substring(0, m.start()) + " " + sig.substring(m.end())).replaceAll("\\s{2,}", " ").trim();
        }
        FunctionDefinitionDataType fd = null;
        try {
            fd = CParserUtils.parseSignature(null, currentProgram, text, false);
        }
        catch (Throwable e) {
            why[0] = String.valueOf(e.getMessage()).split("\n")[0];
        }
        if (fd == null) {
            try {
                fd = new FunctionSignatureParser(currentProgram.getDataTypeManager(), null).parse(null, text);
            }
            catch (Throwable e) {
                if (why[0].isEmpty()) why[0] = String.valueOf(e.getMessage()).split("\n")[0];
            }
        }
        if (fd != null && cc != null) {
            try {
                fd.setCallingConvention(cc);
            }
            catch (Exception e) {
                why[0] = "calling convention " + cc + ": " + e.getMessage();
                return null;
            }
        }
        return fd;
    }

    boolean applySignature(Function f, String sig) {
        String[] why = { "" };
        try {
            FunctionDefinitionDataType fd = parseSignature(sig, why);
            if (fd == null) { fail("signatures: did not parse", f.getEntryPoint() + " " + sig + " -> " + why[0]); return false; }
            ApplyFunctionSignatureCmd cmd = new ApplyFunctionSignatureCmd(f.getEntryPoint(), fd, SourceType.USER_DEFINED, false, false,
                DataTypeConflictHandler.DEFAULT_HANDLER, FunctionRenameOption.NO_CHANGE);
            if (!cmd.applyTo(currentProgram, monitor)) { fail("signatures: Ghidra rejected it", f.getEntryPoint() + " " + sig + " " + cmd.getStatusMsg()); return false; }
            count("signatures applied");
            return true;
        }
        catch (Throwable e) {
            fail("signatures: Ghidra rejected it", f.getEntryPoint() + " " + sig + " -> " + String.valueOf(e.getMessage()).split("\n")[0]);
            return false;
        }
    }

    // ---- data --------------------------------------------------------------------------------
    void applyData(File file) throws Exception {
        Table t = readTable(file, "address", "size", "name", "type", "source", "verified_bos", "verified_bos_hr");
        String verifiedCol = target == 0 ? "verified_bos" : "verified_bos_hr";
        println("DATA: " + t.rows.size() + " rows in the kit");
        DataTypeManager dtm = currentProgram.getDataTypeManager();
        DataTypeParser parser = new DataTypeParser(dtm, dtm, null, DataTypeParser.AllowedDataTypes.ALL);
        PropertyMapManager pmm = currentProgram.getUsrPropertyManager();
        StringPropertyMap record = pmm.getStringPropertyMap(PROPERTY_MAP + ".data");
        if (record == null) record = pmm.createStringPropertyMap(PROPERTY_MAP + ".data");
        for (String[] r : t.rows) {
            monitor.checkCancelled();
            if (!t.get(r, verifiedCol).equals("yes")) { count("data not verified for " + EXE_NAME[target]); continue; }
            Address addr = toAddr(Long.parseLong(t.get(r, "address"), 16));
            String name = t.get(r, "name"), type = t.get(r, "type");
            try {
                List<String> parts = splitName(name);
                String simple = parts.get(parts.size() - 1);
                Namespace ns = namespaceFor(parts.subList(0, parts.size() - 1));
                Symbol have = symbols.getSymbol(simple, addr, ns);
                if (have != null) {
                    count("data labels already present");
                }
                else {
                    Symbol s = symbols.createLabel(addr, simple, ns, SourceType.USER_DEFINED);
                    Symbol primary = symbols.getPrimarySymbol(addr);
                    if (primary != null && !primary.equals(s) && primary.getSource() != SourceType.USER_DEFINED) s.setPrimary();
                    count("data labels created");
                }
                if (!type.equals("undefined")) applyDataType(parser, record, addr, name, type);
            }
            catch (Exception e) {
                fail("data: could not label", addr + " " + name + ": " + e.getMessage());
            }
        }
    }

    void applyDataType(DataTypeParser parser, StringPropertyMap record, Address addr, String name, String type) {
        try {
            DataType dt = parser.parse(type);
            if (dt == null) { fail("data: type not found", addr + " " + name + " " + type); return; }
            Data cur = listing.getDataAt(addr);
            boolean defined = cur != null && cur.isDefined();
            String curText = defined ? cur.getDataType().getDisplayName() : null;
            if (defined && cur.getDataType().isEquivalent(dt)) {
                record.add(addr, type);
                count("data types already applied");
                return;
            }
            String recorded = record.getString(addr);
            if (defined && recorded != null && !recorded.equals(curText) && !overwrite) {
                count("data types kept (you changed it since)");
                return;
            }
            Address end = addr.add(Math.max(dt.getLength(), 1) - 1);
            AddressSet range = new AddressSet(addr, end);
            if (listing.getInstructions(range, true).hasNext()) {
                count("data types kept (code is in the range)");
                return;
            }
            listing.clearCodeUnits(addr, end, false);
            listing.createData(addr, dt);
            record.add(addr, type);
            count("data types applied");
        }
        catch (Exception e) {
            fail("data: could not apply the type", addr + " " + name + " " + type + ": " + e.getMessage());
        }
    }

    // ---- report ------------------------------------------------------------------------------
    void printSummary() {
        println("");
        println("FOT symbol kit: done for " + EXE_NAME[target]);
        for (Map.Entry<String,Integer> e : counts.entrySet()) println(String.format("  %6d  %s", e.getValue(), e.getKey()));
        boolean any = false;
        for (Map.Entry<String,List<String>> e : examples.entrySet()) {
            if (!any) { println("Examples of what was not applied:"); any = true; }
            println("  " + e.getKey());
            for (String s : e.getValue()) println("      " + s);
        }
    }
}
