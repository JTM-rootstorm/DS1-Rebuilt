// Name functions from a TSV of address, name and evidence: the name goes
// on the function, the evidence into its plate comment. Only default names
// (FUN_..., thunk_FUN_...) are replaced; a name someone gave by hand stays
// and is reported. Addresses without a function get no function (that
// would change the lifter's function list), only the reported count.
//
// Headless (writes the project):
//   bash /usr/share/ghidra/support/analyzeHeadless "$PWD" ds1_recomp \
//     -process DungeonSiege.exe -noanalysis \
//     -scriptPath tools/ghidra -postScript ApplyNames.java <names.tsv>
//@category Recomp

import java.io.BufferedReader;
import java.io.FileReader;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ApplyNames extends GhidraScript {
  @Override
  protected void run() throws Exception {
    String[] args = getScriptArgs();
    int named = 0, kept = 0, missing = 0;
    try (BufferedReader r = new BufferedReader(new FileReader(args[0]))) {
      String line;
      while ((line = r.readLine()) != null) {
        String[] p = line.split("\t", 3);
        if (p.length < 3) continue;
        Address a = toAddr(Long.parseLong(p[0], 16));
        Function f = getFunctionAt(a);
        if (f == null) {
          missing++;
          continue;
        }
        String old = f.getName();
        if (!old.equals(p[1]) && f.getSymbol().getSource() != SourceType.DEFAULT
            && !old.startsWith("thunk_FUN_")) {
          println("kept " + p[0] + " " + old + " (not " + p[1] + ")");
          kept++;
          continue;
        }
        f.setName(p[1], SourceType.ANALYSIS);
        currentProgram.getListing().setComment(a, CodeUnit.PLATE_COMMENT, p[2]);
        named++;
      }
    }
    println("ApplyNames: " + named + " named, " + kept + " kept, " + missing
        + " without a function");
  }
}
