#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Debug-app admission: real pure Java + actual Source wrapper; no device."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import debug_gate_test

ANDROID = Path(__file__).resolve().parents[1]
JAVA = ANDROID / 'app/src/main/java/org/s2arm/android'
PROBE = r'''
package org.s2arm.android;
import java.io.*; import java.nio.file.*; import java.util.*;
public class DebugAppProbe {
 static final String BASE="org.s2arm.game.t0100000000abcdef", TEST=BASE+".w2", TITLE="0100000000ABCDEF";
 static int count;
 static void check(boolean b,String why){count++;if(!b)throw new AssertionError(why);}
 public static void main(String[] args) throws Exception {
  Path app=Files.createTempDirectory("debug-app-admission-");
  try {
   Path game=GameSessionAdmissionTest.fixture(app);
   var installed=GameSessionAdmissionTest.apk(app,game);
   try(var session=GameSession.load(GameSessionAdmissionTest.spec(app,game),app.toFile(),installed,false)) {
    check(session!=null,"default release remains admitted");
   }
   GameSessionAdmissionTest.artifact(game,t->t.replace(BASE,TEST));
   var testInstalled=new GameSession.InstalledApk(installed.file,TEST,1,installed.signerSha256,false);
   for(boolean debug:new boolean[]{false,true}) for(boolean gate:new boolean[]{false,true}) {
    boolean accepted=false;
    try(var session=GameSession.load(GameSessionAdmissionTest.spec(app,game),app.toFile(),testInstalled,debug,gate)) {
     accepted=true; check(session.spec.game.titleId.equals(TITLE),"title unchanged");
    } catch(BootContract.BadGame expected) { }
    check(accepted==(debug&&gate),"both debug gates required");
    try(var lock=GameStorage.acquire(app.toFile(),TITLE)) {check(lock!=null,"lock released");}
   }
   for(var wrong:List.of(new GameSession.InstalledApk(installed.file,BASE,1,installed.signerSha256,false),
     new GameSession.InstalledApk(installed.file,TEST,2,installed.signerSha256,false),
     new GameSession.InstalledApk(installed.file,TEST,1,List.of("00".repeat(32)),false),
     new GameSession.InstalledApk(installed.file,TEST,1,installed.signerSha256,true))) {
    boolean refused=false;
    try(var session=GameSession.load(GameSessionAdmissionTest.spec(app,game),app.toFile(),wrong,true,true)) {}
    catch(IOException|BootContract.BadGame expected) {refused=true;}
    check(refused,"complete installed identity retained");
   }
   check(TITLE.equals(BootContract.packageTitle(TEST,true)),"suffix route returns original title only");
   check(TITLE.equals(BootContract.packageTitle(BASE,false)),"default route without gate");
   boolean closed=false;try{BootContract.packageTitle(TEST,false);}catch(IllegalArgumentException e){closed=true;}
   check(closed,"closed gate cannot route test app");
   for(String bad:new String[]{BASE+".foo",BASE+".w2.extra",BASE+".W2",BASE+"..w2",BASE+".2w",BASE+".w2/",BASE+".w2\n","org.s2arm.android"})
    check(BootContract.packageTitle(bad,true)==null,"invalid route "+bad);
   var now=BootContract.readCurrent(game.toFile());
   byte[] artifact=Files.readAllBytes(game.resolve(now.revisionDir).resolve(now.artifact));
   byte[] oldCurrent=Files.readAllBytes(game.resolve("current.json"));
   Path oldSave=app.resolve("user/0/saves/"+TITLE+"/checkpoint");
   Files.createDirectories(oldSave.getParent()); Files.writeString(oldSave,"original synthetic save");
   // Source is the same seam for directory and SAF; the artifact must be read once.
   for(boolean debug:new boolean[]{false,true}) for(boolean gate:new boolean[]{false,true}) {
    int[] reads={0};
    GameInstall.Source source=p->{if(GameInstall.ARTIFACT.equals(p)){reads[0]++;return new ByteArrayInputStream(artifact);}
      return GameInstall.directory(app.resolve("pack").toFile()).open(p);};
    var checked=ImportGuard.checkedImportSource(source,TEST,debug,gate);
    boolean accepted=false;
    try(InputStream in=checked.open(GameInstall.ARTIFACT)){check(Arrays.equals(artifact,in.readAllBytes()),"exact original bytes");accepted=true;}
    catch(IOException e){}
    check(accepted==(debug&&gate),"import both gates");check(reads[0]==1,"artifact read once");
    Path target=app.resolve("fresh-"+debug+"-"+gate);
    if(accepted){
      GameInstall.install(target.resolve("games").toFile(),checked,null);
      check(BootContract.open(target.resolve("games/"+TITLE).toFile()).apk.packageName.equals(TEST),"fresh import keeps exact test package");
    } else {
      boolean refused=false;try{GameInstall.install(target.resolve("games").toFile(),checked,null);}catch(IOException e){refused=true;}
      check(refused&&!Files.exists(target.resolve("games/"+TITLE+"/current.json")),"closed import cannot publish fresh current");
    }
    if(!accepted){
      boolean refused=false;
      try{GameInstall.install(app.resolve("games").toFile(),checked,null);}catch(IOException e){refused=true;}
      check(refused,"installer cannot commit rejected source");
      check(Arrays.equals(oldCurrent,Files.readAllBytes(game.resolve("current.json"))),"old current untouched");
    }
    check(Files.readString(oldSave).equals("original synthetic save"),"original save untouched");
   }
   for(String wrong:new String[]{BASE,"org.s2arm.game.t0100000000000001.w2"}) {
    boolean refused=false;
    try(InputStream in=ImportGuard.checkedImportSource(p->new ByteArrayInputStream(artifact),wrong,true,true).open(GameInstall.ARTIFACT)){}
    catch(IOException e){refused=true;}check(refused,"import exact installed package");
   }
   Files.writeString(installed.file.toPath(),"other APK");boolean refused=false;
   try(var session=GameSession.load(GameSessionAdmissionTest.spec(app,game),app.toFile(),testInstalled,true,true)){}
   catch(IOException e){refused=true;}check(refused,"APK hash still enforced");
   System.out.println("Debug app admission: "+count+" checks PASS; synthetic host, not Android execution");
  } finally {GameInstall.deleteTree(app.toFile());}
 }
}
'''

GATE_PROBE = r'''
package org.s2arm.android;
public class GateAdmissionProbe {
 public static void main(String[] args) throws Exception {
  String title="0100000000ABCDEF", pkg="org.s2arm.game.t0100000000abcdef.w2";
  var cached=DebugGate.class.getDeclaredField("open"); cached.setAccessible(true);
  int count=0;
  for(boolean installedDebug:new boolean[]{false,true}) for(boolean property:new boolean[]{false,true}) {
   // Inject only the cached system-property result. The real BuildConfig check still executes.
   cached.set(null,property);
   boolean accepted=false;
   try { BootContract.requireTestApkPermission(title,pkg,pkg,installedDebug,DebugGate.open()); accepted=true; }
   catch(BootContract.BadGame expected) { }
   if(accepted!=(installedDebug&&BuildConfig.DEBUG&&property)) throw new AssertionError("three authorities");
   String artifact="{\"titleId\":\""+title+"\",\"debug\":true,\"apk\":{\"debuggable\":true,\"packageName\":\""+pkg+"\"}}";
   accepted=false;
   try { BootContract.checkTestImport(artifact.getBytes(java.nio.charset.StandardCharsets.UTF_8),pkg,installedDebug,DebugGate.open()); accepted=true; }
   catch(BootContract.BadGame expected) { }
   if(accepted!=(installedDebug&&BuildConfig.DEBUG&&property)) throw new AssertionError("artifact self-claim is not authority");
   count+=2;
  }
  System.out.println("Real DebugGate build="+BuildConfig.DEBUG+": "+count+" checks PASS (host cached-property injection)");
 }
}
'''


class DebugAppAdmissionTest(unittest.TestCase):
    def test_real_debug_gate_and_installed_flag(self):
        for build_debug in (False, True):
            with self.subTest(build_debug=build_debug), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                sources = [JAVA / (x + '.java') for x in ('Json', 'BootContract', 'DebugGate')]
                for rel, code in debug_gate_test.STUBS.items():
                    path = root / rel
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(code)
                    sources.append(path)
                for name, code in {'BuildConfig': 'package org.s2arm.android; final class BuildConfig { static final boolean DEBUG = %s; }' % str(build_debug).lower(),
                                   'GateAdmissionProbe': GATE_PROBE}.items():
                    path = root / (name + '.java')
                    path.write_text(code)
                    sources.append(path)
                result = subprocess.run(['javac', '-encoding', 'UTF-8', '-d', str(root / 'cls'), *map(str, sources)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run(['java', '-cp', str(root / 'cls'), 'org.s2arm.android.GateAdmissionProbe'], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                print(result.stdout.strip())

    def test_real_admission_and_import_guard(self):
        library = (JAVA / 'GameLibrary.java').read_text()
        start = library.index('    static GameInstall.Source checkedImportSource(')
        end = library.index('    private static String outcome(', start)
        # Compile the exact production wrapper, not a reimplementation.
        wrapper = 'package org.s2arm.android; import java.io.*; class ImportGuard {\n' + library[start:end] + '}\n'
        self.assertEqual(library.count('GameInstall.install(gamesDir, checkedImportSource('), 1,
                         'SAF must use the checked source')
        self.assertIn('source -> checkedImportSource(source, context.getPackageName(),', library)
        install = (JAVA / 'GameInstall.java').read_text()
        self.assertIn('sourceCheck.checked(directory(dir))', install)
        self.assertRegex(library,
                         r'GameSession\.load\(spec,\s*context\.getFilesDir\(\),\s*apk,\s*'
                         r'\(context\.getApplicationInfo\(\)\.flags & android\.content\.pm\.ApplicationInfo\.FLAG_DEBUGGABLE\)'
                         r'\s*!=\s*0,\s*DebugGate\.open\(\),\s*timing,\s*saves::recoverIfNeeded\)')
        main = (JAVA / 'MainActivity.java').read_text()
        self.assertIn('BootContract.packageTitle(getPackageName(),', main)
        self.assertIn('(getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0 && DebugGate.open())', main)
        self.assertNotIn('getPackageName().substring("org.s2arm.game.t".length())', main)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'DebugAppProbe.java').write_text(PROBE)
            (root / 'ImportGuard.java').write_text(wrapper)
            classes = ['Json', 'BootContract', 'BootSpec', 'ProcessBirth', 'RuntimeLibrary',
                       'GamePack', 'GameFingerprint', 'GameStorage', 'GameInstall', 'SaveArchive', 'ImportReceiver',
                       'GameSession', 'GameResume']
            sources = [JAVA / (x + '.java') for x in classes]
            sources += [ANDROID / 'tests/GameSessionAdmissionTest.java', root / 'ImportGuard.java', root / 'DebugAppProbe.java']
            # Host-only Android logging seam; admission/import decisions stay production code.
            log = root / 'android/util/Log.java'
            log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text('package android.util; public class Log { '
                           'public static int i(String tag, String message) { return 0; } '
                           'public static int w(String tag, String message, Throwable error) { return 0; } }')
            sources.append(log)
            run = subprocess.run(['javac', '-encoding', 'UTF-8', '-d', str(root / 'cls'), *map(str, sources)],
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            run = subprocess.run(['java', '-cp', str(root / 'cls'), 'org.s2arm.android.DebugAppProbe'],
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            print(run.stdout.strip())


if __name__ == '__main__':
    unittest.main()
