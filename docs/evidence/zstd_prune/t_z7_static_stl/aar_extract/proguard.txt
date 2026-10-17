# JNI 静态注册按名绑定 (Java_com_zzlfeng_logx_LogX_native*):保住 native 方法名。
-keepclasseswithmembernames class com.zzlfeng.logx.LogX {
    native <methods>;
}
# 回调接口由业务实现,JNI 经 GetMethodID 按名查找:
#   KeyProvider.getKey  "(I)[B"                              (logx_jni.cpp:270)
#   ExportCallback.onExport "(Ljava/lang/String;Ljava/lang/String;J)I" (logx_jni.cpp:440)
#   ErrorHandler.onError 经 Kotlin 侧 onNativeError 分发,同样按名保底。
-keepclassmembers class * implements com.zzlfeng.logx.LogX$KeyProvider {
    byte[] getKey(int);
}
-keepclassmembers class * implements com.zzlfeng.logx.LogX$ExportCallback {
    int onExport(java.lang.String, java.lang.String, long);
}
-keepclassmembers class * implements com.zzlfeng.logx.LogX$ErrorHandler {
    void onError(...);
}
