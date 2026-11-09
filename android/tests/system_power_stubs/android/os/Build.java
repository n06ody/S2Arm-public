package android.os;
public class Build {
    public static class VERSION {
        // 故意非 final：宿主测试要改 SDK_INT 走 <35 分支（真 SDK 是 final，编译期内联无碍）
        public static int SDK_INT = 35;
    }
}
