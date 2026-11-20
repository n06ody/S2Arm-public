#!/usr/bin/env python3
"""Actual library/activity/installer contracts; Android and unused export have strict host stubs."""
from pathlib import Path
import subprocess
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / 'app/src/main/java'
GAME_LIBRARY = JAVA / 'org/s2arm/android/GameLibrary.java'
LIBRARY_ACTIVITY = JAVA / 'org/s2arm/android/LibraryActivity.java'

STUBS = {
'android/os/Parcelable.java': '''package android.os; public interface Parcelable {}''',
'android/os/ParcelFileDescriptor.java': '''package android.os; public class ParcelFileDescriptor implements java.io.Closeable {
 public int getFd(){return -1;} public long getStatSize(){return -1;} public void close(){} }''',
'android/net/LocalServerSocket.java': '''package android.net; public class LocalServerSocket implements java.io.Closeable { public LocalServerSocket(String n) throws java.io.IOException {} public LocalSocket accept() throws java.io.IOException { throw new java.io.IOException("closed"); } public void close() throws java.io.IOException {} }''',
'android/net/LocalSocket.java': '''package android.net; public class LocalSocket implements java.io.Closeable { public Credentials getPeerCredentials() { return new Credentials(); } public java.io.InputStream getInputStream() { return null; } public java.io.OutputStream getOutputStream() { return null; } public void close() throws java.io.IOException {} }''',
'android/net/Credentials.java': '''package android.net; public class Credentials { public int getUid() { return 0; } }''',
'android/net/Uri.java': '''package android.net; public class Uri implements android.os.Parcelable { public static Uri parse(String s){return new Uri();} }''',
'android/os/Build.java': '''package android.os; public class Build { public static class VERSION { public static int SDK_INT=34; } }''',
'android/os/Process.java': '''package android.os; public class Process {
 public static int myPid() {throw new AssertionError("save import unexpectedly called");} }''',
'android/os/Environment.java': '''package android.os; public class Environment { public static boolean allFiles=true;
 public static boolean isExternalStorageManager(){return allFiles;} public static java.io.File getExternalStorageDirectory(){return new java.io.File("/storage/emulated/0");} }''',
'android/Manifest.java': '''package android; public class Manifest { public static class permission { public static final String READ_EXTERNAL_STORAGE="android.permission.READ_EXTERNAL_STORAGE"; } }''',
'android/provider/Settings.java': '''package android.provider; public class Settings { public static final String ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION="a", ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION="b"; }''',
'android/content/ClipData.java': '''package android.content; import android.net.Uri; public class ClipData {
 public static ClipData newRawUri(CharSequence label,Uri uri){throw new AssertionError("diagnostic share unexpectedly called");} }''',
'android/content/SharedPreferences.java': '''package android.content; public interface SharedPreferences {
 boolean getBoolean(String key,boolean fallback); Editor edit();
 interface Editor { Editor putBoolean(String key,boolean value); void apply(); } }''',
'android/database/Cursor.java': '''package android.database; public interface Cursor extends AutoCloseable {
 boolean moveToNext(); String getString(int i); void close(); }''',
'android/content/ContentResolver.java': '''package android.content; import android.net.Uri; import android.database.Cursor;
 public class ContentResolver { public Cursor query(Uri u,String[] c,String a,String[] b,String d){return null;}
 public java.io.InputStream openInputStream(Uri u) throws java.io.IOException {return null;}
 public android.os.ParcelFileDescriptor openFileDescriptor(Uri u,String mode) throws java.io.IOException {throw new AssertionError("mod import unexpectedly called");}
 public java.io.OutputStream openOutputStream(Uri u,String mode) throws java.io.IOException {throw new AssertionError("diagnostic save unexpectedly called");} }''',
'android/content/Context.java': '''package android.content; import java.io.File; import android.content.pm.*;
 public class Context { public static String packageName="fixture"; public static int flags; public final File root; public Context(File root){this.root=root;}
 public File getExternalFilesDir(String s){return new File(root,"external");} public File getFilesDir(){return new File(root,"internal");}
 public Context getApplicationContext(){return this;} public File getCacheDir(){return new File(root,"cache");}
 public File getCodeCacheDir(){return new File(root,"code-cache");}
 public ContentResolver getContentResolver(){return new ContentResolver();} public PackageManager getPackageManager(){return new PackageManager();}
 public String getPackageName(){return packageName;} public ApplicationInfo getApplicationInfo(){ApplicationInfo a=new ApplicationInfo();a.flags=flags;return a;}
 public static final int MODE_PRIVATE=0; private static final java.util.Map<String,SharedPreferences> prefs=new java.util.HashMap<>();
 public SharedPreferences getSharedPreferences(String name,int mode){if(mode!=MODE_PRIVATE)throw new AssertionError("unexpected preference mode");
  String key=root.getAbsolutePath()+"/"+name; return prefs.computeIfAbsent(key,k->new MemoryPreferences());}
 public final String getString(int id,Object... args){if(id>=7&&id<=15||id>=113&&id<=115||id>=134&&id<=138)return "L"+id;   // 游戏库页面文案（115：导入进度；134–138：原地使用）
  if(id>=100&&id<113||id>=130&&id<=133)return id==100?"PLAYER_ERR "+java.util.Arrays.toString(args):"E"+id;   // PlayerError 玩家文案（130–133：原地读取的报错）；116–118 保存诊断不许被要
  throw new AssertionError("diagnostic string unexpectedly requested");}
 private static class MemoryPreferences implements SharedPreferences {
  private final java.util.Map<String,Boolean> values=new java.util.HashMap<>();
  public boolean getBoolean(String key,boolean fallback){return values.getOrDefault(key,fallback);}
  public SharedPreferences.Editor edit(){return new SharedPreferences.Editor(){private final java.util.Map<String,Boolean> staged=new java.util.HashMap<>();
   public SharedPreferences.Editor putBoolean(String key,boolean value){staged.put(key,value);return this;}
   public void apply(){values.putAll(staged);}
  };}
 } }''',
'android/content/pm/ApplicationInfo.java': '''package android.content.pm; public class ApplicationInfo {
 public static final int FLAG_DEBUGGABLE=2; public int flags; public String sourceDir="fixture", nativeLibraryDir=""; public String[] splitSourceDirs; }''',
'android/content/pm/PackageManager.java': '''package android.content.pm; public class PackageManager {
 public static final int GET_SIGNING_CERTIFICATES=1, PERMISSION_GRANTED=0; public PackageInfo getPackageInfo(String n,int f) throws NameNotFoundException {return new PackageInfo();}
 public static class NameNotFoundException extends Exception {} }''',
'android/content/pm/PackageInfo.java': '''package android.content.pm; public class PackageInfo {
 public SigningInfo signingInfo; public long getLongVersionCode(){return 1;} }''',
'android/content/pm/SigningInfo.java': '''package android.content.pm; public class SigningInfo {
 public Signature[] getApkContentsSigners(){return new Signature[0];} }''',
'android/content/pm/Signature.java': '''package android.content.pm; public class Signature { public byte[] toByteArray(){return new byte[0];} }''',
'android/provider/DocumentsContract.java': '''package android.provider; import android.net.Uri;
 public class DocumentsContract { public static String getTreeDocumentId(Uri u){return "";}
 public static Uri buildChildDocumentsUriUsingTree(Uri u,String s){return u;}
 public static Uri buildDocumentUriUsingTree(Uri u,String s){return u;}
 public static final String EXTRA_INITIAL_URI="android.provider.extra.INITIAL_URI";
 public static class Document { public static final String COLUMN_DOCUMENT_ID="id", COLUMN_DISPLAY_NAME="name", COLUMN_MIME_TYPE="type", MIME_TYPE_DIR="dir"; } }''',
'android/util/Log.java': '''package android.util; public class Log { public static final int DEBUG=3; public static int d(String a,String b){return 0;} public static boolean isLoggable(String a,int b){return false;} public static final java.util.List<String> messages=new java.util.concurrent.CopyOnWriteArrayList<>(); public static final java.util.concurrent.BlockingQueue<String> queue=new java.util.concurrent.LinkedBlockingQueue<>(); public static int i(String a,String b){messages.add(b);queue.add(b);return 0;} public static int w(String a,String b,Throwable t){return 0;} public static int e(String a,String b){messages.add(b);queue.add(b);return 0;} }''',
'android/util/DisplayMetrics.java': '''package android.util; public class DisplayMetrics { public float density=1; }''',
'android/content/res/Resources.java': '''package android.content.res; public class Resources { public android.util.DisplayMetrics getDisplayMetrics(){return new android.util.DisplayMetrics();} }''',
'android/os/Bundle.java': '''package android.os; public class Bundle { public String getString(String k){return null;} public void putString(String k,String v){} }''',
'android/content/Intent.java': '''package android.content; import android.net.Uri; import java.util.*; public class Intent {
 public static final String ACTION_OPEN_DOCUMENT_TREE="android.intent.action.OPEN_DOCUMENT_TREE", ACTION_SEND="android.intent.action.SEND", EXTRA_STREAM="android.intent.extra.STREAM";
 public static final String ACTION_OPEN_DOCUMENT="android.intent.action.OPEN_DOCUMENT", ACTION_CREATE_DOCUMENT="android.intent.action.CREATE_DOCUMENT", CATEGORY_OPENABLE="android.intent.category.OPENABLE", EXTRA_TITLE="android.intent.extra.TITLE";
 public static final int FLAG_GRANT_READ_URI_PERMISSION=1; private final Map<String,String> values=new HashMap<>();
 public Intent(){} public Intent(String s){} public Intent(String s,Uri u){} public Intent(Intent i){values.putAll(i.values);}
 public Intent putExtra(String k,String v){values.put(k,v);return this;} public Intent putExtra(String k,boolean v){values.put(k,String.valueOf(v));return this;} public void removeExtra(String k){values.remove(k);}
 public String getStringExtra(String k){return values.get(k);} public boolean hasExtra(String k){return values.containsKey(k);} public Uri getData(){return null;}
 public Intent setType(String type){throw new AssertionError("diagnostic share unexpectedly called");}
 public Intent putExtra(String key,android.os.Parcelable value){throw new AssertionError("diagnostic share unexpectedly called");}
 public Intent addFlags(int flags){throw new AssertionError("diagnostic share unexpectedly called");}
 public void setClipData(ClipData value){throw new AssertionError("diagnostic share unexpectedly called");}
 public static Intent createChooser(Intent intent,CharSequence title){throw new AssertionError("diagnostic share unexpectedly called");}
 public Intent addCategory(String category){throw new AssertionError("diagnostic save unexpectedly called");} }''',
'android/view/View.java': '''package android.view; public class View { public static final int VISIBLE=0, GONE=8; public void setBackgroundColor(int c){} public void setPadding(int a,int b,int c,int d){} private int vis; public void setVisibility(int v){vis=v;} public int getVisibility(){return vis;} public interface OnClickListener {void onClick(View v);} public void setEnabled(boolean enabled){throw new AssertionError("diagnostic export unexpectedly called");}
 private static int nextId=1; private int id; public static int generateViewId(){return nextId++;} public void setId(int v){id=v;} public int getId(){return id;}
 public Object layoutParams; public void setLayoutParams(Object p){layoutParams=p;} }''',
'android/widget/TextView.java': '''package android.widget; import android.view.View; import android.content.Context; import java.util.concurrent.*;
 public class TextView extends View { public static final BlockingQueue<String> messages=new LinkedBlockingQueue<>();
 public volatile String text; public int textId; public TextView(Context c){} public void setText(String s){text=s;messages.add(s);} public void setText(int i){textId=i;} public void setTextSize(float f){} public void setTextColor(int c){} }''',
'android/widget/Button.java': '''package android.widget; import android.content.Context; import android.view.View;
 public class Button extends TextView { public Button(Context c){super(c);} public void setOnClickListener(View.OnClickListener l){} }''',
'android/widget/CompoundButton.java': '''package android.widget; import android.content.Context;
 public class CompoundButton extends Button { public interface OnCheckedChangeListener {void onCheckedChanged(CompoundButton button,boolean checked);}
 private boolean checked; private OnCheckedChangeListener listener; public CompoundButton(Context c){super(c);}
 public boolean isChecked(){return checked;} public void setChecked(boolean value){if(checked!=value){checked=value;if(listener!=null)listener.onCheckedChanged(this,value);}}
 public void setOnCheckedChangeListener(OnCheckedChangeListener value){listener=value;} }''',
'android/widget/CheckBox.java': '''package android.widget; import android.content.Context; public class CheckBox extends CompoundButton { public CheckBox(Context c){super(c);} }''',
'android/widget/LinearLayout.java': '''package android.widget; import android.content.Context; import android.view.View; public class LinearLayout extends View {
 public static final int HORIZONTAL=0, VERTICAL=1; public LinearLayout(Context c){} public void setOrientation(int x){} public void setPadding(int a,int b,int c,int d){}
 public final java.util.List<View> children=new java.util.ArrayList<>(); public void addView(View v){children.add(v);} public void addView(View v,LayoutParams p){children.add(v);v.layoutParams=p;} public void removeAllViews(){children.clear();}
 public static class LayoutParams { public final int width,height; public final float weight; public int marginEnd; public LayoutParams(int w,int h){this(w,h,0);} public LayoutParams(int w,int h,float g){width=w;height=h;weight=g;} public void setMarginEnd(int m){marginEnd=m;} } }''',
'android/widget/RadioButton.java': '''package android.widget; import android.content.Context; public class RadioButton extends CompoundButton { public RadioButton(Context c){super(c);} }''',
'android/widget/RadioGroup.java': '''package android.widget; import android.content.Context; import android.view.View;
 public class RadioGroup extends LinearLayout { public interface OnCheckedChangeListener {void onCheckedChanged(RadioGroup group,int checkedId);}
 private int checked=-1; private OnCheckedChangeListener listener; public RadioGroup(Context c){super(c);}
 public void check(int id){if(checked!=id){checked=id;if(listener!=null)listener.onCheckedChanged(this,id);}} public int getCheckedRadioButtonId(){return checked;}
 public void setOnCheckedChangeListener(OnCheckedChangeListener l){listener=l;} }''',
'android/view/Gravity.java': '''package android.view; public class Gravity { public static final int END=8388613, BOTTOM=80; }''',
'android/widget/FrameLayout.java': '''package android.widget; import android.content.Context; import android.view.View; public class FrameLayout extends View {
 public static class LayoutParams { public LayoutParams(int w,int h){} public LayoutParams(int w,int h,int g){} }
 public FrameLayout(Context c){} public final java.util.List<View> children=new java.util.ArrayList<>(); public void addView(View v){children.add(v);} public void addView(View v,LayoutParams p){children.add(v);} }''',
'org/s2arm/android/LibraryShelf.java': '''package org.s2arm.android; import android.content.Context; import android.view.View; import java.io.File;
 final class LibraryShelf extends View { interface Listener { void play(String titleId); void openGameSettings(); void openControllers(); void openSettings(); }
 static final java.util.List<String> added=new java.util.ArrayList<>();
 LibraryShelf(Context c,Listener l){} void addGame(String titleId,String name,File art){added.add(titleId);} String selectedId(){return null;} void refreshPlaytime(){} }''',
'android/widget/ScrollView.java': '''package android.widget; import android.content.Context; import android.view.View; public class ScrollView extends View {
 public ScrollView(Context c){} public void addView(View v){} }''',
'android/app/Activity.java': '''package android.app; import android.content.*; import android.os.Bundle; import android.content.res.Resources; import android.view.View; import java.io.File;
 public class Activity extends Context { public static File testRoot; public static final int RESULT_OK=-1; private Intent intent=new Intent();
 public Activity(){super(testRoot);} public void onCreate(Bundle b){} protected void onResume(){} protected void onNewIntent(Intent i){}
 protected void onActivityResult(int a,int b,Intent i){} protected void onSaveInstanceState(Bundle b){} protected void onDestroy(){} public Intent getIntent(){return intent;} public void setIntent(Intent i){intent=i;}
 public Resources getResources(){return new Resources();} public void setContentView(View v){} public void startActivityForResult(Intent i,int n){}
 public void startActivity(Intent i){} public void runOnUiThread(Runnable r){r.run();} public boolean isDestroyed(){return false;} public boolean isFinishing(){return false;}
 public int checkSelfPermission(String p){return 0;} public void requestPermissions(String[] p,int r){} public void onRequestPermissionsResult(int r,String[] p,int[] g){} }''',
'android/os/Looper.java': '''package android.os; public class Looper { public static Looper getMainLooper(){return new Looper();} }''',
'android/os/Handler.java': '''package android.os; public class Handler { public Handler(Looper l){} public boolean postDelayed(Runnable r,long ms){return true;} }''',
'android/content/DialogInterface.java': '''package android.content; public interface DialogInterface {
 interface OnClickListener { void onClick(DialogInterface d,int which); }
 interface OnDismissListener { void onDismiss(DialogInterface d); } }''',
'android/app/AlertDialog.java': '''package android.app; import android.content.*;
 public class AlertDialog {
 public void setOnDismissListener(DialogInterface.OnDismissListener listener){throw new AssertionError("save dialog unexpectedly called");}
 public void show(){throw new AssertionError("save dialog unexpectedly called");}
 public static class Builder { public Builder(Context c){}
  public Builder setTitle(int id){return this;} public Builder setMessage(CharSequence m){return this;}
  public Builder setPositiveButton(int id,DialogInterface.OnClickListener l){return this;} public Builder setNegativeButton(int id,DialogInterface.OnClickListener l){return this;}
  public AlertDialog create(){throw new AssertionError("save dialog unexpectedly called");}
  public AlertDialog show(){return new AlertDialog();} } }''',
'org/s2arm/android/R.java': '''package org.s2arm.android; public class R { public static class string { public static final int import_game=1, fps_overlay_show=2, export_diagnostics=3, export_diagnostics_running=4, export_diagnostics_failed=5, export_diagnostics_done=6,
 library_game_starting=7, library_current_game=8, library_switch_title=9, library_switch_message=10, library_switch_confirm=11, library_switch_waiting=12, library_switch_timeout=13, library_launch_failed=14, library_switch_cancel=15, player_err_fmt=100, player_err_import=101, player_err_start=102, player_err_no_space=103, player_err_no_space_next=104, player_err_apk=105, player_err_apk_next=106, player_err_access=107, player_err_access_next=108, player_err_pack=109, player_err_pack_next=110, player_err_other=111, player_err_other_next=112, library_title=113, library_more=114, import_progress=115,
 save_diagnostics=116, save_diagnostics_done=117, save_diagnostics_failed=118,
 player_err_all_files=130, player_err_all_files_next=131, player_err_in_place=132, player_err_in_place_next=133,
 in_place_game=134, in_place_access_title=135, in_place_access_text=136, in_place_access_open=137, in_place_not_local=138,
 last_session_dismiss=119, quality_high=139, quality_saver=140, quality_note=141,
 mods_title=142, mods_for_game=143, mods_no_game=144, mods_import_zip=145, mods_import_folder=146, mods_none=147, mods_item=148, mods_remove=149,
 mods_working=150, mods_imported=151, mods_failed=152, mods_err_not_archive=153, mods_err_too_many=154, mods_err_too_large=155, mods_err_no_romfs=156,
 mods_err_missing=157, mods_err_name_taken=158, mods_note_other_game=159, mods_note_exefs=160, mods_note_empty=161, mods_note=162,
 stall_off_note=163, stall_reopen=164, stall_reopen_failed=165, ldn_settings=166,
 import_save=167, import_save_select_game=168, import_save_reading=169, import_save_summary=170,
 import_save_confirm_title=171, import_save_confirm=172, import_save_access_failed=173, import_save_failed=174, import_save_done=175; } }''',
'org/s2arm/android/NativeBridge.java': '''package org.s2arm.android; final class NativeBridge {
 static RuntimeLibrary loaded(){throw new AssertionError("save import unexpectedly called");}
 static void load(RuntimeLibrary runtime,String apk,String[] splits,String nativeDir) throws RuntimeLibrary.Rejected {
  throw new AssertionError("save import unexpectedly called");}
 static byte[] saveTransfer(byte[] request){throw new AssertionError("save service unexpectedly called");}
 }''',
'org/s2arm/android/AndroidLdnSettings.java': '''package org.s2arm.android; final class AndroidLdnSettings { static void show(android.app.Activity a) {throw new AssertionError("LDN settings unexpectedly called");} }''',
# These unrelated app boundaries must never be called in an import test.
'org/s2arm/android/DiagnosticExport.java': '''package org.s2arm.android; import android.content.Context; import java.io.IOException; import java.nio.file.Path;
 public class DiagnosticExport { public static Path run(Context context) throws IOException {throw new AssertionError("diagnostic export unexpectedly called");} }''',
'org/s2arm/android/DiagnosticShareProvider.java': '''package org.s2arm.android; import android.content.Context; import android.net.Uri; import java.nio.file.Path;
 public class DiagnosticShareProvider { public static Uri uri(Context context,Path path){throw new AssertionError("diagnostic share unexpectedly called");}
 public static Path directory(Context context){throw new AssertionError("diagnostic save unexpectedly called");} }''',
'org/s2arm/android/LastSessionNotice.java': '''package org.s2arm.android; import android.content.Context;
 public class LastSessionNotice { static String text(Context context,long markedAt){throw new AssertionError("last-session notice unexpectedly called");} }''',
'org/s2arm/android/GameResume.java': '''package org.s2arm.android; import java.io.File; import java.io.IOException; public class GameResume { static String activeTitle(File c){return null;}
 public static final String EXTRA="resume"; public static String requireSame(File f,int p,String t,File g) throws IOException,BootContract.BadGame{return t;} }''',
'org/s2arm/android/GameActivity.java': '''package org.s2arm.android; import android.content.*; public class GameActivity { static final String SWITCH_EXTRA="org.s2arm.android.SWITCH_GAME";
 public static int runningPid(Context c){return 0;} public static Intent launch(Context c,Intent i,String t){return i;} }''',
'org/s2arm/android/BootSpecSource.java': '''package org.s2arm.android; import java.io.*; import android.content.Context;
 public class BootSpecSource { public static BootSpec build(Context c,File f,BootContract.Game g) throws IOException {return new BootSpec();} }''',
# Only Android stat wiring is substituted: call the real fingerprint's host NIO
# fallback, retaining production install/session/fingerprint validation and receipts.
'org/s2arm/android/AndroidFileStamp.java': '''package org.s2arm.android; import java.io.*; import java.lang.reflect.*;
public class AndroidFileStamp {
 public static GameFingerprint.Stamp read(File f) throws IOException {
  try { Method m=GameFingerprint.class.getDeclaredMethod("nioStat",File.class); m.setAccessible(true);
   return (GameFingerprint.Stamp)m.invoke(null,f);
  } catch(InvocationTargetException e) { if(e.getCause() instanceof IOException)throw (IOException)e.getCause();throw new IOException(e.getCause());
  } catch(ReflectiveOperationException e) {throw new IOException(e);} }
 public static void probe(File f) throws IOException {read(f);} }''',
'org/s2arm/android/DebugGate.java': 'package org.s2arm.android; class DebugGate { static boolean enabled; static boolean open(){return enabled;} }',
'org/s2arm/android/BuildConfig.java': '''package org.s2arm.android; public class BuildConfig { public static final boolean DEBUG=false; }''',
'org/s2arm/android/ImportProbe.java': '''package org.s2arm.android;
import java.io.*; import java.nio.file.*; import java.lang.reflect.*; import java.util.concurrent.*;
public class ImportProbe {
 static int checks; static boolean refused; static String refusal;
 static void done(BlockingQueue<String> q) throws Exception { refused=false; refusal=null; long end=System.nanoTime()+TimeUnit.SECONDS.toNanos(4);
  // 收尾信号是库清单那行「Games: …」，它只进日志（不当状态发给界面）；界面队列只看拒绝原因，它先于收尾发出，收尾后再排空一遍
  while(System.nanoTime()<end){String s=q.poll(50,TimeUnit.MILLISECONDS); note(s); if(s!=null && s.startsWith("Games:") && q==android.util.Log.queue)return;
   String l=q==android.util.Log.queue?null:android.util.Log.queue.poll(); if(l!=null && l.startsWith("Games:")){for(String r;(r=q.poll())!=null;)note(r);return;}}
  throw new AssertionError("import did not finish"); }
 static void note(String s){if(s!=null && s.startsWith("PLAYER_ERR [E101")){refused=true;refusal=s;}}
 static void demand(boolean yes,String message){checks++;if(!yes)throw new AssertionError(message);}
 static void stage(Path p) throws Exception {Files.createDirectories(p.getParent()); Method m=GameInstallTest.class.getDeclaredMethod("pack",String.class);m.setAccessible(true);Files.move(((File)m.invoke(null,p.toString())).toPath(),p);}
 static boolean retained(Path p){return Files.exists(p.resolve("manifest.json"));}
 public static void main(String[] args) throws Exception {
  // 选了上一级文件夹不挡：根上有包只取根；没有就取所有含 manifest.json 的子目录，嵌在别的包里的不算；一个都没有才是空
  demand(GameLibrary.packageRoots(java.util.List.of("manifest.json","romfs/manifest.json")).equals(java.util.List.of("")),"package at the picked root");
  demand(GameLibrary.packageRoots(java.util.List.of("x.txt","g1/manifest.json","g1/romfs/manifest.json","g2/sub/manifest.json"))
      .equals(java.util.List.of("g1/","g2/sub/")),"packages found below the picked folder, nested ones skipped");
  demand(GameLibrary.packageRoots(java.util.List.of("a/b.txt")).isEmpty(),"no package anywhere");
  Path root=Paths.get(args[0]); Path imports=root.resolve("external/import"),priv=root.resolve("internal/import");
  Path own=imports.resolve("same"),privateOwn=priv.resolve("same"),other=imports.resolve("other");
  stage(own);stage(privateOwn);stage(other);
  BlockingQueue<String> q=new LinkedBlockingQueue<>();
  GameLibrary library=new GameLibrary(new android.content.Context(root.toFile()),text -> q.add(text));
  try {
   library.scanImports();done(q);
   demand(retained(own)&&retained(privateOwn)&&retained(other),"untargeted resume consumed a package");
   library.scanImports(privateOwn.toString());done(q);
   demand(!Files.exists(privateOwn),"private absolute target was not consumed");
   BootContract.Current activated=BootContract.readCurrent(root.resolve("internal/games/0100000000ABCDEF").toFile());
   File gameRoot=root.resolve("internal/games/0100000000ABCDEF").toFile();
   GameFingerprint.Reference fingerprint=GameFingerprint.currentReference(gameRoot,activated.contentRoot.substring(7));
   demand(fingerprint!=null,"explicit import lost the published fingerprint binding");
   File fingerprintTable=new File(new File(gameRoot,activated.revisionDir),fingerprint.path);
   demand(fingerprintTable.isFile()&&GamePack.sha256(Files.readAllBytes(fingerprintTable.toPath())).equals(fingerprint.sha256),"published fingerprint table hash differs from current binding");
   demand(android.util.Log.messages.contains("Game import receipt: 0100000000ABCDEF contentRoot="+activated.contentRoot+" artifactSha256="+activated.artifactSha256+" from import/same"),"successful import lost the active identity receipt");
   demand(retained(own)&&retained(other),"private import consumed external or same-name input");
   library.scanImports(privateOwn.toString());done(q);
   demand(retained(own)&&retained(other),"already consumed target consumed unrelated input");
   library.scanImports(own.toString());done(q);
   demand(!Files.exists(own)&&retained(other),"external absolute target isolation failed");
   library.scanImports();done(q);demand(retained(other),"repeated untargeted resume consumed input");
   for(String invalid:new String[]{"other",imports.resolve("x/../other").toString(),root.resolve("outside").toString(),imports.toString()}) {
    library.scanImports(invalid);done(q);demand(refused&&retained(other),"invalid target consumed other: "+invalid);
   }
   Path outside=root.resolve("outside");stage(outside);
   library.scanImports(outside.toString());done(q);demand(refused&&retained(outside),"outside-root target consumed");
   Path alias=imports.resolve("alias");Files.createSymbolicLink(alias,other.getFileName());
   library.scanImports(alias.toString());done(q);demand(refused&&retained(other)&&Files.isSymbolicLink(alias),"target symlink consumed input");
   Path linked=root.resolve("linked-import");Files.createSymbolicLink(linked,imports);
   library.scanImports(linked.resolve("other").toString());done(q);demand(refused&&retained(other),"parent symlink accepted");
   Path ancestorAlias=imports.resolve("ancestor-alias");Files.createSymbolicLink(ancestorAlias,root.resolve("external"));
   library.scanImports(ancestorAlias.resolve("import/other").toString());done(q);
   demand(refused&&retained(other),"descendant alias escaped direct-child boundary");
   Path crossVictim=priv.resolve("cross-victim");stage(crossVictim);
   Path crossAlias=imports.resolve("cross-alias");Files.createSymbolicLink(crossAlias,root.resolve("internal"));
   library.scanImports(crossAlias.resolve("import/cross-victim").toString());done(q);
   demand(refused&&retained(crossVictim),"cross-root alias escaped direct-child boundary");
   Path systemAlias=root.resolve("sdcard");Files.createSymbolicLink(systemAlias,root.resolve("external"));
   Path aliasTarget=imports.resolve("system-alias-target");stage(aliasTarget);
   library.scanImports(systemAlias.resolve("import/system-alias-target").toString());done(q);
   demand(!Files.exists(aliasTarget)&&retained(other),"trusted-root system alias not accepted or unrelated input consumed");
   Path nested=imports.resolve("nest/target");stage(nested);
   library.scanImports(nested.toString());done(q);demand(refused&&retained(nested),"non-direct child consumed");
   Path payloadLink=priv.resolve("payload-link");stage(payloadLink);Files.delete(payloadLink.resolve("romfs/readme.txt"));
   Files.createSymbolicLink(payloadLink.resolve("romfs/readme.txt"),other.resolve("romfs/readme.txt"));
   library.scanImports(payloadLink.toString());done(q);demand(refused&&retained(payloadLink)&&retained(other),"source symlink accepted or target damaged");
   Path activityRoot=root.resolve("activity"),target=activityRoot.resolve("external/import/selected"),pending=activityRoot.resolve("external/import/pending");
   stage(target);stage(pending);
   android.app.Activity.testRoot=activityRoot.toFile(); LibraryActivity activity=new LibraryActivity();
   android.content.SharedPreferences prefs=activity.getSharedPreferences("display",android.content.Context.MODE_PRIVATE);
   prefs.edit().putBoolean("fps",true).apply();
   android.content.Intent intent=new android.content.Intent().putExtra(LibraryActivity.EXTRA_IMPORT_STAGING,target.toString());
   try {
   activity.setIntent(intent);activity.onCreate(null);
   java.lang.reflect.Field fpsField=LibraryActivity.class.getDeclaredField("fpsOverlay");fpsField.setAccessible(true);
   android.widget.CheckBox fps=(android.widget.CheckBox)fpsField.get(activity);
   demand(fps.isChecked(),"saved FPS preference did not populate checkbox");
   fps.setChecked(false); demand(!prefs.getBoolean("fps",true),"checkbox change did not publish FPS preference");
   prefs.edit().putBoolean("fps",true).apply(); android.util.Log.queue.clear(); activity.onResume();done(android.util.Log.queue);
   demand(fps.isChecked(),"activity resume did not reread FPS preference");
   demand(!Files.exists(target),"activity did not import absolute target");demand(retained(pending),"activity consumed unrelated input");
   demand(intent.getStringExtra(LibraryActivity.EXTRA_IMPORT_STAGING)==null,"activity did not consume intent extra");
   android.util.Log.queue.clear(); activity.onResume();done(android.util.Log.queue);demand(retained(pending),"repeated activity resume consumed input");
   // 被拒的导入：拒绝原因留在状态行上，随后的「Games: …」库清单不能把它盖掉藏起；下次回到页面再收起。
   java.lang.reflect.Field statusField=LibraryActivity.class.getDeclaredField("status");statusField.setAccessible(true);
   android.widget.TextView status=(android.widget.TextView)statusField.get(activity);
   Path refusedTarget=activityRoot.resolve("external/import/nest/target");stage(refusedTarget);   // 不是导入目录的直接子项：拒绝
   activity.setIntent(new android.content.Intent().putExtra(LibraryActivity.EXTRA_IMPORT_STAGING,refusedTarget.toString()));
   android.util.Log.queue.clear(); activity.onResume();done(android.util.Log.queue);
   demand(status.text!=null&&status.text.startsWith("PLAYER_ERR"),"import refusal replaced on the status line: "+status.text);
   demand(status.getVisibility()==android.view.View.VISIBLE,"import refusal hidden on the status line");
   android.util.Log.queue.clear(); activity.onResume();done(android.util.Log.queue);
   demand(status.getVisibility()==android.view.View.GONE,"stale status line not cleared on the next resume");
   } finally {activity.onDestroy();}
   Path failedReceiptRoot=root.resolve("receipt-failure"),receiptTarget=failedReceiptRoot.resolve("external/import/selected");stage(receiptTarget);
   BlockingQueue<String> receiptQueue=new LinkedBlockingQueue<>();
   GameLibrary receiptLibrary=new GameLibrary(new android.content.Context(failedReceiptRoot.toFile()),text -> {
    if(text.startsWith("Game ")&&text.endsWith(" from import/selected")) {
     try {Files.move(failedReceiptRoot.resolve("internal/games/0100000000ABCDEF/current.json"),failedReceiptRoot.resolve("committed-current.json"));}
     catch(IOException e){throw new AssertionError(e);}
    }
    receiptQueue.add(text);
   });
   try {
    android.util.Log.messages.clear();receiptLibrary.scanImports(receiptTarget.toString());done(receiptQueue);
    demand(!refused&&!Files.exists(receiptTarget)&&Files.isRegularFile(failedReceiptRoot.resolve("committed-current.json")),"receipt read failure changed committed import outcome");
    demand(android.util.Log.messages.contains("Game import receipt unavailable: 0100000000ABCDEF from import/selected"),"missing current identity did not report unavailable receipt");
   } finally {receiptLibrary.close();}
   // 原地使用：共享存储上的资源目录只登记、不复制、不消费；回执带 canonical 根（deploy_game.py --in-place 按它认结论）
   Path shared=root.resolve("shared/S2Arm/Games/game");stage(shared);
   String sharedRoot=shared.toRealPath().toString();
   android.util.Log.messages.clear();
   library.registerInPlace(shared.toFile());done(q);
   File inPlaceGame=root.resolve("internal/games/0100000000ABCDEF").toFile();
   BootContract.Game inPlaceBoot=BootContract.open(inPlaceGame);
   demand(!refused&&retained(shared)&&Files.isRegularFile(shared.resolve("romfs/readme.txt")),"in-place registration refused or consumed the folder: "+refusal);
   demand(sharedRoot.equals(inPlaceBoot.inPlaceRoot),"in-place root not recorded: "+inPlaceBoot.inPlaceRoot);
   demand(!Files.exists(inPlaceGame.toPath().resolve(inPlaceBoot.revisionDir).resolve("romfs")),"in-place content was copied into the private revision");
   demand(android.util.Log.messages.contains("Game import receipt: 0100000000ABCDEF contentRoot="+inPlaceBoot.contentRoot
       +" artifactSha256="+inPlaceBoot.artifactSha256+" from in-place "+sharedRoot),"in-place registration lost its identity receipt");
   // 选了上一级（放着几款的文件夹）：逐个直接子目录找包；一个包都没有才拒
   Files.createDirectories(root.resolve("shared/empty/sub"));
   library.registerInPlace(root.resolve("shared/empty").toFile());done(q);
   demand(refused&&refusal.contains("no game package"),"folder without a package not refused: "+refusal);
   library.registerInPlace(root.resolve("shared/S2Arm/Games").toFile());done(q);
   demand(!refused,"parent folder of an in-place package refused: "+refusal);
   // 没开「所有文件访问」时真失败：报错里带这个真原因（PlayerError 归 NO_ALL_FILES = E130 / E131，合 main 后的资源号）
   android.os.Environment.allFiles=false;
   library.registerInPlace(root.resolve("shared/missing").toFile());done(q);
   android.os.Environment.allFiles=true;
   demand(refused&&refusal.startsWith("PLAYER_ERR [E101, E130, E131, All files access is off"),"missing permission not named: "+refusal);
   // 选择器树文档 id → 路径（规则照 Swap2-Native PathUtil）
   File primary=new File("/storage/emulated/0");
   demand("/storage/emulated/0/S2Arm/Games".equals(GameLibrary.inPlacePath("primary:S2Arm/Games",primary)),"primary tree path");
   demand("/storage/emulated/0".equals(GameLibrary.inPlacePath("primary:",primary)),"primary root path");
   demand("/storage/1234-ABCD/Games/x".equals(GameLibrary.inPlacePath("1234-ABCD:Games/x",primary)),"SD card tree path");
   demand("/storage/emulated/0/Download/g".equals(GameLibrary.inPlacePath("raw:/storage/emulated/0/Download/g",primary)),"raw path");
   demand("/storage/emulated/0/S2Arm/g".equals(GameLibrary.inPlacePath("primary:/storage/emulated/0/S2Arm/g",primary)),"whole path from the picker");
   demand(GameLibrary.inPlacePath("msf:123",primary)==null&&GameLibrary.inPlacePath("nocolon",primary)==null,"non-local ids give no path");
   System.out.println("PASS actual GameLibrary, LibraryActivity and GameInstall: "+checks+" checks");
  } finally {library.close();}
 }
}
''',
}

STUBS['org/s2arm/android/W2ImportProbe.java'] = r'''package org.s2arm.android;
import java.io.*; import java.nio.file.*; import java.util.concurrent.*;
public class W2ImportProbe {
 static java.util.Map<String,String> installedState(Path games) throws Exception {
  java.util.Map<String,String> state=new java.util.TreeMap<>();
  if(Files.exists(games)) try(java.util.stream.Stream<Path> paths=Files.walk(games)) {
   for(Path path:(Iterable<Path>)paths::iterator) {
    String name=games.relativize(path).toString();
    if(name.isEmpty()||name.equals(".session.lock"))continue;
    state.put(name,Files.isRegularFile(path)?GamePack.sha256(Files.readAllBytes(path)):"directory");
   }
  }
  return state;
 }
 public static void main(String[] args) throws Exception {
  int checks=0;String title="0100000000abcdef",pkg="org.s2arm.game.t"+title;
  for(String inbox:new String[]{"internal","external"}) for(int scenario=0;scenario<9;scenario++) {
   Path root=Paths.get(args[0],inbox+scenario),target=root.resolve(inbox+"/import/selected");ImportProbe.stage(target);
   String artifactPackage=pkg+".w2";
   android.content.Context.packageName=artifactPackage;android.content.Context.flags=2;DebugGate.enabled=true;
   if(scenario==1)android.content.Context.flags=0;
   if(scenario==2)DebugGate.enabled=false;
   if(scenario==3)artifactPackage="org.s2arm.game.t0100000000abcdee.w2";
   if(scenario==4)artifactPackage=pkg+".other";
   if(scenario==5)android.content.Context.packageName=pkg;
   if(scenario==6){artifactPackage=pkg;android.content.Context.packageName=pkg;android.content.Context.flags=0;DebugGate.enabled=false;}
   if(scenario==8) {
    GameInstall.install(root.resolve("internal/games").toFile(),GameInstall.directory(target.toFile()),null);
    android.content.Context.flags=0;
   }
   java.util.Map<String,String> before=installedState(root.resolve("internal/games"));
   Path artifact=target.resolve(GameInstall.ARTIFACT);
   String text=Files.readString(artifact),hash="0".repeat(64);
   String apk="\"apk\":{\"packageName\":\""+artifactPackage+"\",\"versionCode\":1,\"sha256\":\""+hash+"\",\"signingCertSha256\":\""+hash+"\",\"assetRoot\":\"game/modules\"},";
   Files.writeString(artifact,"{"+apk+text.substring(1));
   // scenario 7 在本线程直接调 importDirectory：不建 GameLibrary（构造时后台清扫持存储锁，与本线程取锁赛跑，偶发「game storage is busy」）
   BlockingQueue<String> q=new LinkedBlockingQueue<>();GameLibrary library=scenario==7?null:new GameLibrary(new android.content.Context(root.toFile()),q::add);
   try {
    if(scenario==7) {
     ImportProbe.refused=false;
     GameInstall.importDirectory(root.resolve("internal").toFile(),
      GameInstall.importDirectories(root.resolve("internal").toFile(),root.resolve("external").toFile()),target.toString(),null,
      new GameInstall.ImportListener() {
       public void installed(File source,GameInstall.Result result) { }
       public void refused(File source,Exception error) {ImportProbe.refused=true;}
      });
    } else {library.scanImports(target.toString());ImportProbe.done(q);}
    boolean accepted=scenario==0||scenario==6;
    Path game=root.resolve("internal/games/0100000000ABCDEF");
    if(accepted) {
     if(ImportProbe.refused||Files.exists(target)||!Files.isRegularFile(game.resolve("current.json")))throw new AssertionError("authorized import failed "+inbox+scenario+" "+q);
     if(!BootContract.open(game.toFile()).titleId.equals("0100000000ABCDEF"))throw new AssertionError("wrong routed title");
    } else {
     if(!ImportProbe.refused||!ImportProbe.retained(target))throw new AssertionError("unauthorized import accepted "+inbox+scenario);
     // The storage lease creates games/.session.lock; no installation-owned child may be written.
     if(!installedState(root.resolve("internal/games")).equals(before))throw new AssertionError("unauthorized artifact wrote installation state "+inbox+scenario);
    }
    checks++;
   } finally {if(library!=null)library.close();}
   try(GameStorage lease=GameStorage.acquire(root.resolve("internal").toFile(),"0100000000ABCDEF")) {checks++;}
  }
  System.out.println("PASS W2 explicit import: "+checks+" checks");
 }
}
'''

class GameLibraryImportTest(unittest.TestCase):
    def run_contract(self, automatic_scan=False, missing_receipt=False, missing_fingerprint=False, w2=False, bypass_admission=False, preferences_not_published=False, library_as_status=False):
        with tempfile.TemporaryDirectory(prefix='s2a-library-import-') as temporary:
            base = Path(temporary)
            sources = []
            for relative, content in STUBS.items():
                if preferences_not_published and relative == 'android/content/Context.java':
                    before = 'public void apply(){values.putAll(staged);}'
                    self.assertEqual(content.count(before), 1)
                    content = content.replace(before, 'public void apply(){}')
                path = base / 'src' / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(textwrap.dedent(content), encoding='utf-8')
                sources.append(str(path))
            library = GAME_LIBRARY
            if automatic_scan:
                # Effective negative control: restore untargeted queue traversal in a copy.
                original = 'void scanImports() { scanImports(null); }'
                replacement = ('void scanImports() { for (File inbox : importDirs) { '
                               'File[] targets = inbox.listFiles(); if (targets != null) '
                               'for (File target : targets) scanImports(target.getAbsolutePath()); } }')
                text = GAME_LIBRARY.read_text(encoding='utf-8')
                self.assertEqual(text.count(original), 1)
                library = base / 'GameLibrary.java'
                library.write_text(text.replace(original, replacement), encoding='utf-8')
            if missing_receipt:
                # Effective merge regression: retaining only the old callback loses main's receipt.
                text = GAME_LIBRARY.read_text(encoding='utf-8')
                start = text.index('                            // Preserve the deployment receipt')
                end = text.index('                        }\n                        @Override public void refused', start)
                library = base / 'GameLibrary.java'
                library.write_text(text[:start] + text[end:], encoding='utf-8')
            if library_as_status:
                # Effective negative control: send the library listing to the screen again (pre-fix behaviour).
                text = GAME_LIBRARY.read_text(encoding='utf-8')
                original = 'Log.i("S2Arm", describe());'
                self.assertEqual(text.count(original), 1)
                library = base / 'GameLibrary.java'
                library.write_text(text.replace(original, 'status(describe());'), encoding='utf-8')
            if missing_fingerprint:
                install = JAVA / 'org/s2arm/android/GameInstall.java'
                text = install.read_text(encoding='utf-8')
                original = 'result.previousContentRoot, fingerprint);'
                self.assertEqual(text.count(original), 1)
                mutant = base / 'GameInstall.java'
                mutant.write_text(text.replace(original, 'result.previousContentRoot, null);'), encoding='utf-8')
                sources.append(str(mutant))
            if bypass_admission:
                install = JAVA / 'org/s2arm/android/GameInstall.java'
                text = install.read_text(encoding='utf-8')
                original = 'sourceCheck.checked(directory(dir))'
                self.assertEqual(text.count(original), 1)
                mutant = base / 'GameInstall.java'
                mutant.write_text(text.replace(original, 'directory(dir)'), encoding='utf-8')
                sources.append(str(mutant))
            compiled = subprocess.run(
                ['javac', '-encoding', 'UTF-8', '-d', str(base / 'classes'), '-sourcepath', str(JAVA),
                 str(library), str(LIBRARY_ACTIVITY), str(ROOT / 'tests/GameInstallTest.java'), *sources],
                capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            return subprocess.run(
                ['java', '-Djava.io.tmpdir=' + str(base), '-cp', str(base / 'classes'),
                 'org.s2arm.android.W2ImportProbe' if w2 else 'org.s2arm.android.ImportProbe', str(base / 'data')],
                capture_output=True, text=True, timeout=20)

    def test_preference_apply_negative_control(self):
        result = self.run_contract(preferences_not_published=True)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('saved FPS preference did not populate checkbox', result.stderr)

    def test_bypassed_w2_admission_negative_control(self):
        result = self.run_contract(w2=True, bypass_admission=True)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('unauthorized import accepted internal1', result.stderr)

    def test_w2_admission_before_install_writes(self):
        result = self.run_contract(w2=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS W2 explicit import: 36 checks', result.stdout)

    def test_only_explicit_absolute_target_is_consumed(self):
        result = self.run_contract()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS actual GameLibrary, LibraryActivity and GameInstall: 49 checks', result.stdout)

    def test_library_listing_as_status_negative_control(self):
        result = self.run_contract(library_as_status=True)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('import refusal replaced on the status line', result.stderr)

    def test_missing_fingerprint_negative_control(self):
        result = self.run_contract(missing_fingerprint=True)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('explicit import lost the published fingerprint binding', result.stderr)

    def test_missing_receipt_negative_control(self):
        result = self.run_contract(missing_receipt=True)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('successful import lost the active identity receipt', result.stderr)

    def test_automatic_scan_negative_control(self):
        result = self.run_contract(automatic_scan=True)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('untargeted resume consumed a package', result.stderr)

if __name__ == '__main__':
    unittest.main(verbosity=2)
