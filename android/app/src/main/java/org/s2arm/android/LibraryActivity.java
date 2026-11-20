package org.s2arm.android;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.File;
import java.io.IOException;
import java.util.HashSet;
import java.util.Set;

/** The library never loads the native runtime. Each explicit launch is admitted again in :game. */
public final class LibraryActivity extends Activity {
    private GameLibrary library;
    private TextView status;
    // #528 item 15: "the last game session ended abnormally", once, above the status line; never blocks the library.
    private LinearLayout notice;
    private TextView noticeText;
    // 提示排在封面流下面（和它同在一列），占地方而不是盖在卡片的游戏名上；「更多」面板打开时右边让出面板的宽度
    private LinearLayout.LayoutParams noticeParams;
    private int panelWidth;
    private LinearLayout more;
    private android.widget.ScrollView moreScroll;
    private LibraryShelf shelf;
    private CheckBox fpsOverlay;
    private final Set<String> listed = new HashSet<>();
    private final java.util.Map<String, String> names = new java.util.HashMap<>();
    private final android.os.Handler ui = new android.os.Handler(android.os.Looper.getMainLooper());
    private static final int PICK_PACKAGE = 1, PICK_IN_PLACE = 2, ASK_LEGACY_READ = 3, SAVE_DIAGNOSTICS = 4,
        PICK_MOD_ZIP = 5, PICK_MOD_DIR = 6, PICK_SAVE = 7;
    // 资源替换类 mod（GameMods）：面板打开时正中那张卡片的那一款；一次只做一件（导入 / 开关 / 删除）
    private String modsTitle;
    private boolean modsBusy;
    private TextView modsGame;
    private Button modsZip, modsFolder;
    private LinearLayout modsList;
    private LinearLayout stallRow;   // 停帧救场关掉的帧率补丁：有标记才显示（StallOff）
    /** 原地使用的约定位置：文件夹选择器从这里打开（在的话）；deploy_game.py --in-place 也推到这下面。 */
    static final String IN_PLACE_DIR = "S2Arm/Games";
    // Bundle name waiting for the system file picker (kept across activity recreation).
    private String pendingSave;
    private static final String STATE_PENDING_SAVE = "pendingDiagnosticSave";
    private static final String STATE_SAVE_TITLE = "pendingSaveTitle";
    private String pendingSaveTitle;
    private Button importSaveButton;
    // A caller opts in to consuming one direct child of files/import/.
    public static final String EXTRA_IMPORT_STAGING = "org.s2arm.android.IMPORT_STAGING";
    /** 电脑直传资源包（ImportReceiver）：开本地套接字 s2arm-import.&lt;包名&gt;，只收 adb（shell uid）连进来的一次传输。 */
    public static final String EXTRA_IMPORT_RECEIVE = "org.s2arm.android.IMPORT_RECEIVE";
    static final int SHELL_UID = 2000;          // adb forward 的连接由 adbd 发起，对端是 shell 用户
    static final int RECEIVE_WAIT_MS = 120000;  // 开着等电脑连进来的最长时间

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        if (state != null) {
            pendingSave = state.getString(STATE_PENDING_SAVE);
            pendingSaveTitle = state.getString(STATE_SAVE_TITLE);
        }
        // 与 iOS 同一个样子（shell/Sources/ShellUI/CoverFlowView.swift）：横屏封面流，点正中的卡片开始玩。
        //   导入 / 帧率显示 / 诊断包不在主页面：右上「游戏设置」「设置」两个圆钮打开右侧面板（iOS 的设置页还没整套搬过来，先放这三项）。
        float density = getResources().getDisplayMetrics().density;
        android.widget.FrameLayout root = new android.widget.FrameLayout(this);
        more = new LinearLayout(this); more.setOrientation(LinearLayout.VERTICAL);
        int pad = Math.round(20 * density);
        more.setPadding(pad, pad, pad, pad);
        // 面板项多了（加了 mod 一节）横屏放不下：整块可滚
        moreScroll = new android.widget.ScrollView(this); moreScroll.setVisibility(android.view.View.GONE);
        moreScroll.setBackgroundColor(0xF01A1A1C);
        moreScroll.addView(more);
        shelf = new LibraryShelf(this, new LibraryShelf.Listener() {
            @Override public void play(String titleId) { launch(titleId); }
            @Override public void openGameSettings() { toggleMore(); }
            @Override public void openControllers() { toggleMore(); }
            @Override public void openSettings() { toggleMore(); }
        });
        // 封面流（连同浮在它底部的状态行）占满余下的高度；「上一局异常结束」提示排在它下面，没有提示时高度为 0，封面流照旧满屏。
        //   状态行照旧浮在卡片上（#1230 维护者选的做法：不为导入进度让卡片一缩一放）
        android.widget.FrameLayout stage = new android.widget.FrameLayout(this);
        stage.addView(shelf, new android.widget.FrameLayout.LayoutParams(-1, -1));
        LinearLayout column = new LinearLayout(this); column.setOrientation(LinearLayout.VERTICAL);
        column.addView(stage, new LinearLayout.LayoutParams(-1, 0, 1f));
        root.addView(column, new android.widget.FrameLayout.LayoutParams(-1, -1));
        // 停帧救场关掉的帧率补丁（StallOff）：正中那款有标记才出这一行（放在面板最上面：面板要滚动，放底下玩家看不到），「重新打开」删标记、下次起局照常开
        stallRow = new LinearLayout(this); stallRow.setOrientation(LinearLayout.VERTICAL);
        TextView stallNote = new TextView(this); stallNote.setText(R.string.stall_off_note); stallNote.setTextColor(0xFFFFFFFF);
        stallRow.addView(stallNote);
        Button stallReopen = new Button(this); stallReopen.setText(R.string.stall_reopen);
        stallReopen.setOnClickListener(v -> reopenStall());
        stallRow.addView(stallReopen);
        stallRow.setVisibility(android.view.View.GONE);
        more.addView(stallRow);
        Button importButton = new Button(this); importButton.setText(R.string.import_game);
        importButton.setOnClickListener(v -> startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_PACKAGE));
        more.addView(importButton);
        // 原地使用：游戏文件已经在手机存储里（文件管理器 / 电脑拷进去），直接用那里的，不再拷一份进 App
        Button inPlaceButton = new Button(this); inPlaceButton.setText(R.string.in_place_game);
        inPlaceButton.setOnClickListener(v -> useInPlace());
        more.addView(inPlaceButton);
        android.content.SharedPreferences display = getSharedPreferences("display", MODE_PRIVATE);
        fpsOverlay = new CheckBox(this);
        fpsOverlay.setText(R.string.fps_overlay_show);
        fpsOverlay.setTextColor(0xFFFFFFFF);
        fpsOverlay.setChecked(display.getBoolean("fps", false));
        fpsOverlay.setOnCheckedChangeListener((button, checked) -> {
            if (display.getBoolean("fps", false) != checked)
                display.edit().putBoolean("fps", checked).apply();
        });
        more.addView(fpsOverlay);
        // 画质：高画质（打开 EASU 等上屏增强）/ 更省电（缺省）。存在 display 偏好 quality_high，游戏进程开局时读（MainActivity → NativeBridge.configureHighQuality）
        android.widget.RadioGroup quality = new android.widget.RadioGroup(this);
        quality.setOrientation(android.widget.RadioGroup.HORIZONTAL);
        android.widget.RadioButton high = new android.widget.RadioButton(this), saver = new android.widget.RadioButton(this);
        high.setId(android.view.View.generateViewId()); saver.setId(android.view.View.generateViewId());
        high.setText(R.string.quality_high); saver.setText(R.string.quality_saver);
        high.setTextColor(0xFFFFFFFF); saver.setTextColor(0xFFFFFFFF);
        quality.addView(high); quality.addView(saver);
        quality.check(display.getBoolean("quality_high", false) ? high.getId() : saver.getId());
        quality.setOnCheckedChangeListener((group, id) -> {
            boolean on = id == high.getId();
            if (display.getBoolean("quality_high", false) != on) display.edit().putBoolean("quality_high", on).apply();
        });
        more.addView(quality);
        android.widget.TextView qualityNote = new android.widget.TextView(this);
        qualityNote.setText(R.string.quality_note); qualityNote.setTextColor(0xFFB0B0B0); qualityNote.setTextSize(12);
        more.addView(qualityNote);
        Button ldnButton = new Button(this); ldnButton.setText(R.string.ldn_settings);
        ldnButton.setOnClickListener(v -> AndroidLdnSettings.show(this));
        more.addView(ldnButton);
        // #528 item 15: works without any game; the bundle goes to whatever app the player picks.
        Button exportButton = new Button(this); exportButton.setText(R.string.export_diagnostics);
        exportButton.setOnClickListener(v -> exportDiagnostics(exportButton, false));
        more.addView(exportButton);
        // Some share sheets (Xiaomi's, for one) have no "save as file" target: the system file picker always does.
        Button saveButton = new Button(this); saveButton.setText(R.string.save_diagnostics);
        saveButton.setOnClickListener(v -> exportDiagnostics(saveButton, true));
        more.addView(saveButton);
        importSaveButton = new Button(this);
        importSaveButton.setText(R.string.import_save);
        importSaveButton.setOnClickListener(v -> pickSave());
        more.addView(importSaveButton);
        // 资源替换类 mod（口径同 pack/jobs/mods.py）：按游戏放，落到这款起局用的 state 目录 mods/（GameMods.modsDir），下次起局叠上
        TextView modsHeader = new TextView(this); modsHeader.setText(R.string.mods_title); modsHeader.setTextColor(0xFFFFFFFF);
        more.addView(modsHeader);
        modsGame = new TextView(this); modsGame.setTextColor(0xFFB0B0B0); modsGame.setTextSize(12);
        more.addView(modsGame);
        modsZip = new Button(this); modsZip.setText(R.string.mods_import_zip);
        modsZip.setOnClickListener(v -> startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
            .addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), PICK_MOD_ZIP));
        more.addView(modsZip);
        modsFolder = new Button(this); modsFolder.setText(R.string.mods_import_folder);
        modsFolder.setOnClickListener(v -> startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_MOD_DIR));
        more.addView(modsFolder);
        modsList = new LinearLayout(this); modsList.setOrientation(LinearLayout.VERTICAL);
        more.addView(modsList);
        TextView modsNote = new TextView(this);
        modsNote.setText(R.string.mods_note); modsNote.setTextColor(0xFFB0B0B0); modsNote.setTextSize(12);
        more.addView(modsNote);
        panelWidth = Math.round(320 * density);
        root.addView(moreScroll, new android.widget.FrameLayout.LayoutParams(panelWidth, -1, android.view.Gravity.END));   // 在 column 之后加：面板盖在提示和状态行上面
        notice = new LinearLayout(this); notice.setOrientation(LinearLayout.VERTICAL); notice.setVisibility(android.view.View.GONE);
        notice.setPadding(pad, pad / 2, pad, pad / 2); notice.setBackgroundColor(0xE6332A12);
        noticeText = new TextView(this); noticeText.setTextColor(0xFFFFFFFF);
        notice.addView(noticeText);
        // 入口复用「更多」面板里的导出 / 保存：同一条路径、同一个按钮的忙碌状态
        LinearLayout noticeActions = new LinearLayout(this);
        Button noticeExport = new Button(this); noticeExport.setText(R.string.export_diagnostics);
        noticeExport.setOnClickListener(v -> { hideNotice(); exportDiagnostics(exportButton, false); });
        Button noticeSave = new Button(this); noticeSave.setText(R.string.save_diagnostics);
        noticeSave.setOnClickListener(v -> { hideNotice(); exportDiagnostics(saveButton, true); });
        Button noticeDismiss = new Button(this); noticeDismiss.setText(R.string.last_session_dismiss);
        noticeDismiss.setOnClickListener(v -> hideNotice());
        noticeActions.addView(noticeExport); noticeActions.addView(noticeSave); noticeActions.addView(noticeDismiss);
        notice.addView(noticeActions);
        noticeParams = new LinearLayout.LayoutParams(-1, -2);
        column.addView(notice, noticeParams);
        // 状态行只给玩家看得懂的：导入进度、失败原因；库清单（Games: 文件数 / 哈希…）只进日志，不上屏
        status = new TextView(this); status.setVisibility(android.view.View.GONE);
        status.setTextColor(0xFFFFFFFF); status.setPadding(pad, pad / 2, pad, pad / 2); status.setBackgroundColor(0xB3000000);
        stage.addView(status, new android.widget.FrameLayout.LayoutParams(-1, -2, android.view.Gravity.BOTTOM));
        setContentView(root);
        library = new GameLibrary(getApplicationContext(), new GameLibrary.Listener() {
            @Override public void onStatus(String text) { runOnUiThread(() -> {
                if (isDestroyed()) return;   // 库清单只进日志（GameLibrary.logLibrary），这里收到的都是给玩家看的状态
                status.setText(text);
                status.setVisibility(android.view.View.VISIBLE);
            }); }
            @Override public void onGameAvailable(String titleId, String name, java.io.File art) { runOnUiThread(() -> {
                if (isDestroyed() || !listed.add(titleId)) return;
                names.put(titleId, name);
                shelf.addGame(titleId, name, art);
            }); }
        });
    }

    private void toggleMore() {
        boolean open = moreScroll.getVisibility() == android.view.View.GONE;
        moreScroll.setVisibility(open ? android.view.View.VISIBLE : android.view.View.GONE);
        // 面板开着时提示让出右边：三个按钮都露在面板左边，面板最下面几行也点得到
        noticeParams.setMarginEnd(open ? panelWidth : 0);
        notice.setLayoutParams(noticeParams);
        if (open) openMods(shelf.selectedId());
    }

    // ---- 资源替换类 mod ----

    private interface ModsJob { GameMods.Result run(File home, String titleId) throws IOException, GameMods.Failure; }

    /** 面板打开：对准正中那款，列出它的 mod。 */
    private void openMods(String titleId) {
        modsTitle = titleId;
        stallRow.setVisibility(titleId != null && StallOff.marked(getFilesDir(), titleId) ? android.view.View.VISIBLE : android.view.View.GONE);
        modsList.removeAllViews();
        if (titleId == null) {
            modsGame.setText(getString(R.string.mods_no_game));
            setModsEnabled(false);
            return;
        }
        modsGame.setText(getString(R.string.mods_for_game, label(titleId)));
        setModsEnabled(!modsBusy);
        if (!modsBusy) mods(null);
    }

    /** 重新打开停帧救场关掉的帧率补丁：删这款的标记，删不掉在状态行说真原因、这一行留着。 */
    private void reopenStall() {
        String titleId = modsTitle;
        if (titleId == null) return;
        String why = StallOff.reopen(getFilesDir(), titleId);
        if (why != null) { show(getString(R.string.stall_reopen_failed, why)); return; }
        android.util.Log.i("S2Arm", "帧率补丁重新打开：删了 " + titleId + " 的停帧救场标记，下次起局照常开");
        stallRow.setVisibility(StallOff.marked(getFilesDir(), titleId) ? android.view.View.VISIBLE : android.view.View.GONE);
    }

    private void setModsEnabled(boolean on) { modsZip.setEnabled(on); modsFolder.setEnabled(on); }

    /** 后台做一件 mod 的事（job 为 null = 只列出），做完在状态行说结果、重列这款的 mod。 */
    private void mods(ModsJob job) {
        final String titleId = modsTitle;
        if (titleId == null || modsBusy) return;
        modsBusy = true;
        setModsEnabled(false);
        if (job != null) {   // 做的时候不留可点的行：做完按真状态重列
            show(getString(R.string.mods_working));
            modsList.removeAllViews();
        }
        final File files = getFilesDir();
        new Thread(() -> {
            String line = null;
            java.util.List<GameMods.Mod> list = null;
            try {
                File home = GameMods.modsDir(files, titleId);
                if (job != null) {
                    GameMods.Result r = job.run(home, titleId);
                    if (r != null) line = describeImport(r);
                }
                list = GameMods.listing(home);
            } catch (GameMods.Failure f) {
                line = getString(R.string.mods_failed, modsText(f.message));
            } catch (IOException | RuntimeException e) {
                line = getString(R.string.mods_failed, e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName());
            }
            if (list == null) {
                try { list = GameMods.listing(GameMods.modsDir(files, titleId)); } catch (IOException | RuntimeException ignored) { }
            }
            final String done = line;
            final java.util.List<GameMods.Mod> shown = list;
            runOnUiThread(() -> {
                if (isDestroyed()) return;
                modsBusy = false;
                if (done != null) show(done);
                if (!titleId.equals(modsTitle)) return;   // 面板已换到别的款
                setModsEnabled(true);
                renderMods(titleId, shown);
            });
        }, "s2a-mods").start();
    }

    private String describeImport(GameMods.Result r) {
        StringBuilder sb = new StringBuilder(r.imported.isEmpty() ? "" : getString(R.string.mods_imported, String.join(" / ", r.imported)));
        for (GameMods.Message note : r.notes) sb.append(sb.length() == 0 ? "" : "\n").append(modsText(note));
        return sb.toString();
    }

    private String modsText(GameMods.Message m) {
        int id;
        switch (m.kind) {
            case NOT_ARCHIVE: id = R.string.mods_err_not_archive; break;
            case TOO_MANY_FILES: id = R.string.mods_err_too_many; break;
            case TOO_LARGE: id = R.string.mods_err_too_large; break;
            case NO_ROMFS: id = R.string.mods_err_no_romfs; break;
            case MISSING: id = R.string.mods_err_missing; break;
            case NAME_TAKEN: id = R.string.mods_err_name_taken; break;
            case OTHER_GAME: id = R.string.mods_note_other_game; break;
            case EXEFS_SKIPPED: id = R.string.mods_note_exefs; break;
            default: id = R.string.mods_note_empty; break;
        }
        return getString(id, m.args);
    }

    private void renderMods(String titleId, java.util.List<GameMods.Mod> list) {
        modsList.removeAllViews();
        if (list == null) return;
        if (list.isEmpty()) {
            TextView none = new TextView(this); none.setText(getString(R.string.mods_none)); none.setTextColor(0xFFB0B0B0);
            modsList.addView(none);
            return;
        }
        for (GameMods.Mod m : list) {
            LinearLayout row = new LinearLayout(this); row.setOrientation(LinearLayout.HORIZONTAL);
            CheckBox on = new CheckBox(this);
            on.setText(getString(R.string.mods_item, m.name, m.files, GameMods.size(m.bytes)));
            on.setTextColor(0xFFFFFFFF);
            on.setChecked(m.enabled);   // 先摆状态再挂回调：摆状态不算玩家点
            on.setOnCheckedChangeListener((button, checked) -> mods((home, id) -> { GameMods.setEnabled(home, m.name, checked); return null; }));
            Button remove = new Button(this); remove.setText(R.string.mods_remove);
            remove.setOnClickListener(v -> mods((home, id) -> { GameMods.remove(home, m.name); return null; }));
            row.addView(on); row.addView(remove);
            modsList.addView(row);
        }
    }

    /** zip：能定位的普通文件直接按路径读（/proc/self/fd），读不了定位的（网盘之类给的是管道）先拷进 Work。 */
    private GameMods.Result importModZip(android.net.Uri uri, File home, String titleId) throws IOException, GameMods.Failure {
        android.content.ContentResolver cr = getContentResolver();
        String shown = displayName(cr, uri);
        try (GameMods.Work work = new GameMods.Work(home);
             android.os.ParcelFileDescriptor fd = cr.openFileDescriptor(uri, "r")) {
            if (fd == null) throw new IOException("cannot open the picked file");
            File zip = new File("/proc/self/fd/" + fd.getFd());
            if (fd.getStatSize() < 0) {
                zip = new File(work.dir, "picked.zip");
                try (java.io.InputStream in = cr.openInputStream(uri)) {
                    if (in == null) throw new IOException("cannot open the picked file");
                    java.nio.file.Files.copy(in, zip.toPath());
                }
            }
            File root = new File(work.dir, "z");
            GameMods.unpackZip(zip, shown, root);
            return GameMods.importRoot(home, root, shown, GameMods.baseName(shown, true), titleId);
        }
    }

    /** 文件夹（系统选择器的树）：整棵拷进 Work 再找 romfs（选的就是 romfs 时临时目录也叫 romfs）。 */
    private GameMods.Result importModTree(android.net.Uri tree, File home, String titleId) throws IOException, GameMods.Failure {
        android.content.ContentResolver cr = getContentResolver();
        String rootId = android.provider.DocumentsContract.getTreeDocumentId(tree);
        String path = GameLibrary.inPlacePath(rootId, android.os.Environment.getExternalStorageDirectory());
        String shown = path != null ? new File(path).getName()
            : displayName(cr, android.provider.DocumentsContract.buildDocumentUriUsingTree(tree, rootId));
        java.util.Map<String, String> ids = new java.util.HashMap<>();
        java.util.Set<String> dirs = new java.util.HashSet<>();
        GameLibrary.walk(cr, tree, ids, dirs);
        try (GameMods.Work work = new GameMods.Work(home)) {
            File root = new File(work.dir, shown.equalsIgnoreCase("romfs") ? "romfs" : "z");
            java.nio.file.Files.createDirectories(root.toPath());
            for (java.util.Map.Entry<String, String> e : ids.entrySet()) {
                File dst = GameMods.inside(root, e.getKey());
                if (dst == null) continue;
                if (dirs.contains(e.getKey())) { java.nio.file.Files.createDirectories(dst.toPath()); continue; }
                java.nio.file.Files.createDirectories(dst.getParentFile().toPath());
                try (java.io.InputStream in = cr.openInputStream(android.provider.DocumentsContract.buildDocumentUriUsingTree(tree, e.getValue()))) {
                    if (in == null) throw new IOException("cannot read " + e.getKey());
                    java.nio.file.Files.copy(in, dst.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                }
            }
            return GameMods.importRoot(home, root, shown, GameMods.baseName(path != null ? path : shown, false), titleId);
        }
    }

    private static String displayName(android.content.ContentResolver cr, android.net.Uri uri) {
        try (android.database.Cursor c = cr.query(uri, new String[] {android.provider.DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToNext() && c.getString(0) != null) return c.getString(0);
        }
        return "mod";
    }

    private void show(String text) { status.setText(text); status.setVisibility(android.view.View.VISIBLE); }

    // #528 item 15: the last game session died in the foreground or its shutdown never finished ("fg" / "end" in LastSession, the marker the :game process keeps).
    //   Asked once: the marker is cleared before the notice shows. Not while a game process lives (split screen can
    //   keep the game in front next to the library); "bg" or no marker never asks.
    private void checkLastSession() {
        java.io.File dir = getFilesDir();
        if (LastSession.read(dir) != LastSession.FOREGROUND || GameActivity.runningPid(this) != 0) return;
        long markedAt = LastSession.markedAt(dir);
        try { LastSession.mark(dir, LastSession.NONE); }
        catch (java.io.IOException | RuntimeException error) { android.util.Log.w("S2Arm", "Cannot clear the session mark", error); }
        android.content.Context app = getApplicationContext();
        new Thread(() -> {
            String text = LastSessionNotice.text(app, markedAt);   // a binder call for the exit reason: off the main thread
            runOnUiThread(() -> {
                if (isDestroyed()) return;
                noticeText.setText(text);
                notice.setVisibility(android.view.View.VISIBLE);
            });
        }, "s2a-last-session").start();
    }

    private void hideNotice() { notice.setVisibility(android.view.View.GONE); }

    private void launch(String titleId) { launch(titleId, 0); }

    // 不堵门：点的就是正在跑的那款 → 回到它；那款还在启动、收条没写出 → 等一会儿再回到它；
    //   正在跑的是别的款 → 问玩家要不要结束它（照常收局保存）再起这一款，而不是报「launch refused」
    private void launch(String titleId, int waited) {
        Intent options = new Intent(getIntent());
        options.removeExtra(GameResume.EXTRA);
        options.removeExtra(GameActivity.SWITCH_EXTRA);
        int pid = GameActivity.runningPid(this);
        if (pid != 0) {
            try {
                options.putExtra(GameResume.EXTRA,
                    GameResume.requireSame(getCacheDir(), pid, titleId, new java.io.File(getFilesDir(), "games")));
            } catch (java.io.IOException | BootContract.BadGame | RuntimeException notSame) {
                String active = GameResume.activeTitle(getCacheDir());
                if ((active == null || active.equals(titleId)) && waited < 20) {   // 还在启动：每 0.5 秒再看一次，最多 10 秒
                    show(getString(R.string.library_game_starting));
                    ui.postDelayed(() -> { if (!isDestroyed()) launch(titleId, waited + 1); }, 500);
                    return;
                }
                confirmSwitch(active, titleId);
                return;
            }
        }
        try {
            startActivity(GameActivity.launch(this, options, titleId));
        } catch (RuntimeException error) {
            status.setTextColor(0xFFFF5252);
            show(getString(R.string.library_launch_failed, String.valueOf(error.getMessage())));
        }
    }

    private String label(String titleId) {
        if (titleId == null) return getString(R.string.library_current_game);
        String name = names.get(titleId);
        return name != null ? name : titleId;
    }

    private void confirmSwitch(String active, String titleId) {
        new android.app.AlertDialog.Builder(this)
            .setTitle(R.string.library_switch_title)
            .setMessage(getString(R.string.library_switch_message, label(active), label(titleId)))
            .setPositiveButton(R.string.library_switch_confirm, (dialog, which) -> switchTo(active, titleId))
            .setNegativeButton(R.string.library_switch_cancel, null)
            .show();
    }

    // 让正在跑的那局正常收局（MainActivity.onNewIntent 认 SWITCH_EXTRA），等它的进程退出再起新款；20 秒还没退就如实说
    private void switchTo(String active, String titleId) {
        Intent stop = new Intent(getIntent());
        stop.removeExtra(GameResume.EXTRA);
        stop.putExtra(GameActivity.SWITCH_EXTRA, true);
        try {
            startActivity(GameActivity.launch(this, stop, titleId));
        } catch (RuntimeException error) {
            status.setTextColor(0xFFFF5252);
            show(getString(R.string.library_launch_failed, String.valueOf(error.getMessage())));
            return;
        }
        show(getString(R.string.library_switch_waiting, label(active)));
        waitThenLaunch(titleId, 0);
    }

    private void waitThenLaunch(String titleId, int tries) {
        if (isDestroyed()) return;
        if (GameActivity.runningPid(this) == 0) { launch(titleId); return; }
        if (tries >= 40) {
            status.setTextColor(0xFFFF5252);
            show(getString(R.string.library_switch_timeout));
            return;
        }
        ui.postDelayed(() -> waitThenLaunch(titleId, tries + 1), 500);
    }

    private void exportDiagnostics(Button button, boolean saveToFile) {
        button.setEnabled(false);
        show(getString(R.string.export_diagnostics_running));
        android.content.Context app = getApplicationContext();
        new Thread(() -> {
            java.nio.file.Path bundle = null;
            String error = null;
            try {
                bundle = DiagnosticExport.run(app);
            } catch (java.io.IOException | RuntimeException e) {
                error = e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName();
            }
            java.nio.file.Path done = bundle;
            String failure = error;
            runOnUiThread(() -> {
                if (isDestroyed()) return;
                button.setEnabled(true);
                if (done == null) {
                    show(getString(R.string.export_diagnostics_failed, failure));
                    return;
                }
                if (saveToFile) {
                    pendingSave = done.getFileName().toString();
                    Intent create = new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
                        .setType("application/zip").putExtra(Intent.EXTRA_TITLE, pendingSave);
                    startActivityForResult(create, SAVE_DIAGNOSTICS);
                    return;
                }
                android.net.Uri uri = DiagnosticShareProvider.uri(this, done);
                Intent send = new Intent(Intent.ACTION_SEND).setType("application/zip")
                    .putExtra(Intent.EXTRA_STREAM, uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                send.setClipData(android.content.ClipData.newRawUri(done.getFileName().toString(), uri));
                show(getString(R.string.export_diagnostics_done, done.getFileName().toString()));
                startActivity(Intent.createChooser(send, getString(R.string.export_diagnostics)));
            });
        }, "s2a-diag-export").start();
    }

    @Override protected void onResume() {
        super.onResume();
        fpsOverlay.setChecked(getSharedPreferences("display", MODE_PRIVATE).getBoolean("fps", false));
        // 回到页面先收起上一轮的状态行（启动中、切换等待、诊断已导出、导入结果…），再重新扫描。
        status.setVisibility(android.view.View.GONE);
        if (shelf != null) shelf.refreshPlaytime();   // 刚玩完回来：重读游玩时间
        if (library != null) {
            boolean receive = getIntent().hasExtra(EXTRA_IMPORT_RECEIVE);
            getIntent().removeExtra(EXTRA_IMPORT_RECEIVE);
            String staging = getIntent().getStringExtra(EXTRA_IMPORT_STAGING);
            getIntent().removeExtra(EXTRA_IMPORT_STAGING); // do not repeat on later resumes
            if (receive) receiveImport(); else library.scanImports(staging);
        }
        checkLastSession();
    }
    @Override protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state);
        if (pendingSave != null) state.putString(STATE_PENDING_SAVE, pendingSave);
        if (pendingSaveTitle != null) state.putString(STATE_SAVE_TITLE, pendingSaveTitle);
    }

    /** Copies the exported bundle into the document the player created in the system file picker. */
    private void saveDiagnostics(android.net.Uri target, String name) {
        android.content.Context app = getApplicationContext();
        new Thread(() -> {
            String error = null;
            try {
                DiagnosticBundleWriter.saveBundle(DiagnosticShareProvider.directory(app), name,
                    mode -> app.getContentResolver().openOutputStream(target, mode));
            } catch (java.io.IOException | RuntimeException e) {
                error = e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName();
            }
            String failure = error;
            runOnUiThread(() -> {
                if (isDestroyed()) return;
                show(failure == null ? getString(R.string.save_diagnostics_done, name) : getString(R.string.save_diagnostics_failed, failure));
            });
        }, "s2a-diag-save").start();
    }

    @Override protected void onNewIntent(Intent intent) { super.onNewIntent(intent); setIntent(intent); }

    private void pickSave() {
        String title = shelf.selectedId();
        if (title == null) {
            show(getString(R.string.import_save_select_game));
            return;
        }
        pendingSaveTitle = title;
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
            .addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), PICK_SAVE);
    }

    private java.io.File saveStaging() { return new java.io.File(getCacheDir(), "save-import"); }

    private SaveImport saveImport() throws RuntimeLibrary.Rejected {
        android.content.pm.ApplicationInfo app = getApplicationInfo();
        RuntimeLibrary runtime = RuntimeLibrary.select(null, false, getFilesDir(), getCodeCacheDir(),
            android.os.Process.myPid(), NativeBridge.loaded(), () -> GameSession.apkSha256(new java.io.File(app.sourceDir)));
        NativeBridge.load(runtime, app.sourceDir, app.splitSourceDirs, app.nativeLibraryDir);
        return new SaveImport(getFilesDir(), java.util.Locale.getDefault().toLanguageTag(), NativeBridge::saveTransfer);
    }

    private String saveErrorCode(Throwable error) {
        if (error instanceof SaveImport.Failure failure && failure.code.matches("save_[a-z_]+")) return failure.code;
        if (error instanceof GameStorage.Busy) return "save_busy";
        if (error instanceof BootContract.BadGame) return "save_target";
        if (error instanceof UnsupportedOperationException || error instanceof LinkageError || error instanceof RuntimeLibrary.Rejected)
            return "save_unsupported_runtime";
        return "save_import_failed";
    }

    private void showSaveError(Throwable error) {
        String code = saveErrorCode(error);
        android.util.Log.e("S2Arm", "Save import refused " + code);
        String detail = error instanceof SaveImport.Failure ? error.getMessage()
            : getString(R.string.import_save_access_failed);
        show(getString(R.string.import_save_failed, code, detail));
    }

    private static void discardSave(java.nio.file.Path path) {
        if (path == null) return;
        try { java.nio.file.Files.deleteIfExists(path); }
        catch (java.io.IOException error) { android.util.Log.w("S2Arm", "Save import staging cleanup failed", error); }
    }

    private void inspectSave(String title, android.net.Uri uri) {
        importSaveButton.setEnabled(false);
        show(getString(R.string.import_save_reading));
        new Thread(() -> {
            java.nio.file.Path staged = null;
            try {
                try (java.io.InputStream input = getContentResolver().openInputStream(uri)) {
                    if (input == null) throw new java.io.IOException("save_open");
                    staged = SaveArchive.stage(saveStaging().toPath(), input, -1);
                }
                SaveImport.Preview preview = saveImport().inspect(title, staged);
                runOnUiThread(() -> {
                    if (isDestroyed() || isFinishing()) discardSave(preview.archive);
                    else confirmSave(preview);
                });
            } catch (java.io.IOException | BootContract.BadGame | RuntimeLibrary.Rejected | RuntimeException | LinkageError error) {
                discardSave(staged);
                runOnUiThread(() -> {
                    if (isDestroyed()) return;
                    importSaveButton.setEnabled(true);
                    showSaveError(error);
                });
            }
        }, "s2a-save-inspect").start();
    }

    private void confirmSave(SaveImport.Preview preview) {
        StringBuilder message = new StringBuilder(getString(R.string.import_save_summary,
            label(preview.title), preview.files, preview.bytes));
        for (Object warning : preview.warnings) message.append("\n\n").append(warning);
        boolean[] confirmed = {false};
        android.app.AlertDialog dialog = new android.app.AlertDialog.Builder(this)
            .setTitle(R.string.import_save_confirm_title)
            .setMessage(message)
            .setNegativeButton(R.string.library_switch_cancel, null)
            .setPositiveButton(R.string.import_save_confirm, (dismissed, which) -> {
                confirmed[0] = true;
                restoreSave(preview);
            })
            .create();
        dialog.setOnDismissListener(dismissed -> {
            if (!confirmed[0]) {
                discardSave(preview.archive);
                importSaveButton.setEnabled(true);
            }
        });
        dialog.show();
    }

    private void restoreSave(SaveImport.Preview preview) {
        new Thread(() -> {
            try {
                SaveImport.Receipt receipt = saveImport().restore(preview);
                runOnUiThread(() -> {
                    if (!isDestroyed()) show(getString(R.string.import_save_done, receipt.files));
                });
            } catch (java.io.IOException | BootContract.BadGame | RuntimeLibrary.Rejected | RuntimeException | LinkageError error) {
                runOnUiThread(() -> { if (!isDestroyed()) showSaveError(error); });
            } finally {
                discardSave(preview.archive);
                runOnUiThread(() -> { if (!isDestroyed()) importSaveButton.setEnabled(true); });
            }
        }, "s2a-save-restore").start();
    }

    private void restoreReceivedSave(ImportReceiver.Received got, java.io.OutputStream output) throws java.io.IOException {
        SaveImport.Receipt receipt;
        try {
            receipt = saveImport().restoreUnattended(got.saveTitle, got.dir.toPath());
        } catch (java.io.IOException | BootContract.BadGame | RuntimeLibrary.Rejected | RuntimeException | LinkageError error) {
            String code = saveErrorCode(error);
            android.util.Log.e("S2Arm", "Save import refused " + code);
            ImportReceiver.reply(output, "ERR " + code);
            return;
        } finally {
            discardSave(got.dir.toPath());
        }
        String json = receipt.json();
        ImportReceiver.reply(output, "OK " + json);
        android.util.Log.i("S2Arm", "Save import receipt " + json);
    }

    // 直传：本地套接字收一次资源包进私有 files/import/<名字>，收完照常 scanImports（按 index.tsv 逐文件核）。
    //   不经共享存储（FUSE）。只认 shell uid 的对端：别的 App 连进来直接断开、接着等
    private void receiveImport() {
        final GameLibrary lib = library;
        final String name = "s2arm-import." + getPackageName();
        final java.io.File parent = new java.io.File(getFilesDir(), "import");
        new Thread(() -> {
            android.net.LocalServerSocket server;
            try { server = new android.net.LocalServerSocket(name); }
            catch (java.io.IOException e) { android.util.Log.e("S2Arm", "Game import receiver: cannot listen on " + name + ": " + e.getMessage()); return; }
            final android.net.LocalServerSocket srv = server;
            Thread timer = new Thread(() -> {
                try { Thread.sleep(RECEIVE_WAIT_MS); srv.close(); } catch (InterruptedException | java.io.IOException ignored) { }
            }, "s2a-import-wait");
            timer.setDaemon(true); timer.start();
            android.util.Log.i("S2Arm", "Game import receiver listening: " + name);
            try {
                while (true) {
                    android.net.LocalSocket s;
                    try { s = srv.accept(); } catch (java.io.IOException closed) { android.util.Log.e("S2Arm", "Game import receiver: nobody connected"); return; }
                    try (android.net.LocalSocket c = s) {
                        if (c.getPeerCredentials().getUid() != SHELL_UID) {
                            android.util.Log.e("S2Arm", "Game import receiver: refused peer uid " + c.getPeerCredentials().getUid());
                            continue;
                        }
                        timer.interrupt();
                        long t0 = System.nanoTime();
                        ImportReceiver.Received got = ImportReceiver.receiveAny(c.getInputStream(), c.getOutputStream(),
                            parent, saveStaging());
                        if (got.saveTitle != null) {
                            restoreReceivedSave(got, c.getOutputStream());
                            return;
                        }
                        if (got.inPlace) {   // 电脑已把资源目录 adb push 到共享存储：原地登记，不传字节
                            android.util.Log.i("S2Arm", "Game in-place request: " + got.dir);
                            runOnUiThread(() -> lib.registerInPlace(got.dir));
                            return;
                        }
                        java.io.File dir = got.dir;
                        android.util.Log.i("S2Arm", "Game import received " + dir.getName() + " in " + (System.nanoTime() - t0) / 1000000 + " ms");
                        runOnUiThread(() -> lib.scanImports(dir.getAbsolutePath()));
                        return;
                    } catch (java.io.IOException e) {
                        android.util.Log.e("S2Arm", "Game import of requested directory refused: receive failed: " + e.getMessage());
                        return;
                    }
                }
            } finally {
                try { srv.close(); } catch (java.io.IOException ignored) { }
            }
        }, "s2a-import-receive").start();
    }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == PICK_PACKAGE && result == RESULT_OK && data != null && data.getData() != null) library.importTree(data.getData());
        if (request == PICK_IN_PLACE && result == RESULT_OK && data != null && data.getData() != null) {
            String path;
            try { path = GameLibrary.inPlacePath(android.provider.DocumentsContract.getTreeDocumentId(data.getData()),
                android.os.Environment.getExternalStorageDirectory()); }
            catch (IllegalArgumentException notTree) { path = null; }
            if (path == null) { status.setTextColor(0xFFFF5252); show(getString(R.string.in_place_not_local)); return; }
            library.registerInPlace(new java.io.File(path));
        }
        if ((request == PICK_MOD_ZIP || request == PICK_MOD_DIR) && result == RESULT_OK && data != null && data.getData() != null) {
            android.net.Uri picked = data.getData();
            mods(request == PICK_MOD_ZIP ? (home, id) -> importModZip(picked, home, id) : (home, id) -> importModTree(picked, home, id));
        }
        if (request == SAVE_DIAGNOSTICS) {
            String name = pendingSave;
            pendingSave = null;
            if (result == RESULT_OK && data != null && data.getData() != null && name != null) saveDiagnostics(data.getData(), name);
        }
        if (request == PICK_SAVE) {
            String title = pendingSaveTitle;
            pendingSaveTitle = null;
            if (title != null && result == RESULT_OK && data != null && data.getData() != null)
                inspectSave(title, data.getData());
        }
    }

    // 原地使用：读共享存储上别处来的文件要「所有文件访问」（安卓 11+）；没开就说明并带去系统设置页
    //   （做法照 mamcjr/Swap2-Native MainActivity.needAccess / openAccessSettings，MIT）。安卓 10 走旧的读存储权限
    private void useInPlace() {
        if (android.os.Build.VERSION.SDK_INT >= 30 && !android.os.Environment.isExternalStorageManager()) {
            new android.app.AlertDialog.Builder(this)
                .setTitle(R.string.in_place_access_title)
                .setMessage(getString(R.string.in_place_access_text))
                .setPositiveButton(R.string.in_place_access_open, (dialog, which) -> openAllFilesAccess())
                .setNegativeButton(R.string.library_switch_cancel, null)
                .show();
            return;
        }
        if (android.os.Build.VERSION.SDK_INT < 30 && checkSelfPermission(android.Manifest.permission.READ_EXTERNAL_STORAGE)
                != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] {android.Manifest.permission.READ_EXTERNAL_STORAGE}, ASK_LEGACY_READ);
            return;
        }
        pickInPlace();
    }

    private void openAllFilesAccess() {
        try {
            startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                android.net.Uri.parse("package:" + getPackageName())));
        } catch (RuntimeException noPerAppPage) {   // 个别系统没有按应用的那一页：打开总列表
            startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    private void pickInPlace() {
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        if (new java.io.File(android.os.Environment.getExternalStorageDirectory(), IN_PLACE_DIR).isDirectory())
            pick.putExtra(android.provider.DocumentsContract.EXTRA_INITIAL_URI, android.net.Uri.parse(
                "content://com.android.externalstorage.documents/document/primary%3A" + IN_PLACE_DIR.replace("/", "%2F")));
        startActivityForResult(pick, PICK_IN_PLACE);
    }

    @Override public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(request, permissions, results);
        if (request == ASK_LEGACY_READ && results.length == 1 && results[0] == android.content.pm.PackageManager.PERMISSION_GRANTED)
            pickInPlace();
    }
    @Override protected void onDestroy() { if (library != null) library.close(); super.onDestroy(); }
}
