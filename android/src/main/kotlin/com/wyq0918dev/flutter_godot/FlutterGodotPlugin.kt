package com.wyq0918dev.flutter_godot

import android.app.Activity
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.util.Log
import android.view.View
import android.widget.FrameLayout
import androidx.annotation.Keep
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleOwner
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.embedding.engine.plugins.FlutterPlugin.FlutterPluginBinding
import io.flutter.embedding.engine.plugins.activity.ActivityAware
import io.flutter.embedding.engine.plugins.activity.ActivityPluginBinding
import io.flutter.embedding.engine.plugins.lifecycle.FlutterLifecycleAdapter
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.plugin.common.StandardMessageCodec
import io.flutter.plugin.platform.PlatformView
import io.flutter.plugin.platform.PlatformViewFactory
import org.godotengine.godot.Godot
import org.godotengine.godot.GodotHost
import org.godotengine.godot.error.Error
import org.godotengine.godot.plugin.GodotPlugin
import org.godotengine.godot.plugin.SignalInfo
import org.godotengine.godot.plugin.UsedByGodot
import org.godotengine.godot.utils.ProcessPhoenix
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean

/** FlutterGodotPlugin */
class FlutterGodotPlugin : FlutterPlugin, ActivityAware, MethodChannel.MethodCallHandler,
    EventChannel.StreamHandler {

    /** FlutterHost生命周期 */
    private var mLifecycle: Lifecycle? = null

    /** Flutter Host Activity */
    private var mActivity: Activity? = null

    /** Flutter 方法通道 */
    private lateinit var mMethodChannel: MethodChannel

    /** Flutter 事件通道 */
    private lateinit var mEventChannel: EventChannel

    /** Godot实例 */
    private var mGodot: Godot? = null

    /** 初始化上下文 */
    private var initializationContext: Context? = null

    /** Flutter asset lookup */
    private var mFlutterAssets: FlutterPlugin.FlutterAssets? = null

    /** Godot容器 */
    private lateinit var mGodotContainer: FrameLayout

    /** 事件通道事件接收器 */
    private var mEventSink: EventChannel.EventSink? = null

    /** 事件通道监听状态 */
    private val mListeningState = AtomicBoolean(false)

    /** 命令行参数 */
    private val commandLineParams = ArrayList<String>()

    /** 资产名称 */
    private var mAssetName: String? = null

    /** 父级Host */
    private var mParentHost: GodotHost? = null

    private val mSignalInfo: SignalInfo = SignalInfo(SIGNAL_NAME, String::class.java)

    /***
     * 插件附加到 Flutter 引擎
     */
    override fun onAttachedToEngine(binding: FlutterPluginBinding) {
        // 初始化方法通道
        mMethodChannel = MethodChannel(binding.binaryMessenger, GODOT_METHOD_ID)
        // 初始化事件通道
        mEventChannel = EventChannel(binding.binaryMessenger, GODOT_EVENT_ID)
        // 注册平台View
        binding.platformViewRegistry.registerViewFactory(GODOT_VIEW_ID, mFactory)
        // 设置事件通道流处理
        mEventChannel.setStreamHandler(this@FlutterGodotPlugin)
        // 设置方法通道调用处理
        mMethodChannel.setMethodCallHandler(this@FlutterGodotPlugin)
        // Save Flutter asset lookup reference
        mFlutterAssets = binding.flutterAssets
    }

    /**
     * 插件从 Flutter 引擎分离
     */
    override fun onDetachedFromEngine(binding: FlutterPluginBinding) {
        // 清除方法通道调用处理
        mMethodChannel.setMethodCallHandler(null)
        // 清除事件通道流处理
        mEventChannel.setStreamHandler(null)
        mFlutterAssets = null
    }

    /**
     * 插件附加到 Activity
     */
    override fun onAttachedToActivity(binding: ActivityPluginBinding) {
        mActivity = binding.activity
        mLifecycle = FlutterLifecycleAdapter.getActivityLifecycle(binding)

        commandLineParams.addAll(
            elements = binding.activity.intent.getStringArrayExtra(
                EXTRA_COMMAND_LINE_PARAMS
            ) ?: emptyArray()
        )

        if (binding.activity is GodotHost) mParentHost = binding.activity as GodotHost

        binding.addRequestPermissionsResultListener { requestCode, permissions, grantResults ->
            mHost.godot.onRequestPermissionsResult(requestCode, permissions, grantResults)
            return@addRequestPermissionsResultListener true
        }
        binding.addActivityResultListener { requestCode, resultCode, data ->
            mHost.godot.onActivityResult(requestCode, resultCode, data)
            return@addActivityResultListener true
        }
    }

    /**
     * 插件从 Activity 分离
     */
    override fun onDetachedFromActivityForConfigChanges() {
        onDetachedFromActivity()
    }

    /**
     * 插件重新附加到 Activity
     */
    override fun onReattachedToActivityForConfigChanges(binding: ActivityPluginBinding) {
        onAttachedToActivity(binding)
    }

    /**
     * 插件从 Activity 分离
     */
    override fun onDetachedFromActivity() {
        mActivity = null
        mLifecycle = null
        mParentHost = null
    }

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "sendData2Godot" -> call.argument<String>("data").let { data ->
                if (data != null && mGodot != null) GodotPlugin.emitSignal(
                    mGodot,
                    PLUGIN_NAME,
                    mSignalInfo,
                    data,
                ).run {
                    result.success(true)
                } else result.error(
                    "MISSING_DATA_OR_INSTANCE_NULL",
                    "缺少数据参数或实例为空",
                    null,
                )
            }

            else -> result.notImplemented()
        }
    }

    override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
        if (events != null) mEventSink = events
        else {
            Log.e(TAG, "EventSink 为空")
        }
        if (mListeningState.getAndSet(events != null)) {
            Log.w(TAG, "已在监听事件，忽略重复请求")
            return
        }
    }

    /**
     * 消息通道取消监听
     */
    override fun onCancel(arguments: Any?) {
        if (mListeningState.getAndSet(false)) {
            mEventSink = null
        }
    }

    private val mFactory: PlatformViewFactory by lazy {
        return@lazy object : PlatformViewFactory(StandardMessageCodec.INSTANCE) {
            override fun create(context: Context, viewId: Int, args: Any?): PlatformView {
                return object : PlatformView {
                    init {
                        val rawAssetName = (args as? Map<*, *>)?.get(key = ASSET_NAME_KEY) as? String
                        // Extract Flutter asset to filesystem so Godot can access it
                        mAssetName = rawAssetName?.let { extractFlutterAsset(context, it) }
                        mGodotContainer = FrameLayout(context).apply {
                            id = viewId
                        }
                        if (mGodot != null && initializationContext != context) {
                            mHost.onNewGodotInstanceRequested(emptyArray())
                        } else {
                            mLifecycle?.addObserver(mObserver)
                        }
                    }

                    override fun getView(): View = mGodotContainer
                    override fun dispose(): Unit = mGodot?.destroyAndKillProcess() ?: Unit
                }
            }
        }
    }

    private val mHost: GodotHost = object : GodotHost {

        override fun getActivity(): Activity {
            return mActivity!!
        }

        override fun getGodot(): Godot {
            if (mGodot == null) {
                initializationContext = mActivity
                mGodot = Godot(mActivity!!)
            }
            return mGodot!!
        }

        override fun onNewGodotInstanceRequested(args: Array<String>): Int {
            mHost.godot.destroyAndKillProcess()
            ProcessPhoenix.triggerRebirth(
                mActivity,
                Bundle(),
                Intent().apply {
                    setComponent(ComponentName(mActivity!!, mActivity!!.javaClass.name))
                    addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    addFlags(Intent.FLAG_ACTIVITY_CLEAR_TASK)
                    putExtra(EXTRA_COMMAND_LINE_PARAMS, args)
                    putExtra(KEY_SHOW_GODOT_VIEW, true)
                },
            )
            return DEFAULT_WINDOW_ID
        }

        override fun getHostPlugins(engine: Godot): MutableSet<GodotPlugin> {
            return mutableSetOf<GodotPlugin>().apply {
                addAll(elements = super.getHostPlugins(engine))
                add(element = mGodotPlugin)
                mParentHost?.let { host ->
                    addAll(elements = host.getHostPlugins(engine))
                }
            }
        }

        override fun getCommandLine(): List<String?>? {
            return mutableListOf<String?>().apply {
                addAll(elements = super.getCommandLine())
                mParentHost?.let { host ->
                    addAll(elements = host.commandLine)
                }
                addAll(elements = commandLineParams)
                mAssetName?.let { asset ->
                    add(element = "--main-pack")
                    add(element = asset)
                }
            }
        }

        override fun onGodotSetupCompleted() {
            super.onGodotSetupCompleted()
            mParentHost?.onGodotSetupCompleted()
        }

        override fun onGodotMainLoopStarted() {
            super.onGodotMainLoopStarted()
            mParentHost?.onGodotMainLoopStarted()
        }

        override fun onGodotForceQuit(instance: Godot?) {
            super.onGodotForceQuit(instance)
            mParentHost?.onGodotForceQuit(instance)
        }


        override fun onGodotForceQuit(godotInstanceId: Int): Boolean {
            return mParentHost != null && mParentHost!!.onGodotForceQuit(godotInstanceId)
        }

        override fun onGodotRestartRequested(instance: Godot?) {
            super.onGodotRestartRequested(instance)
            mParentHost?.onGodotRestartRequested(instance)
        }

        override fun signApk(
            inputPath: String,
            outputPath: String,
            keystorePath: String,
            keystoreUser: String,
            keystorePassword: String
        ): Error? {
            return mParentHost?.signApk(
                inputPath, outputPath, keystorePath, keystoreUser, keystorePassword
            )
            return Error.ERR_UNAVAILABLE
        }

        override fun verifyApk(apkPath: String): Error? {
            return mParentHost?.verifyApk(apkPath)
            return Error.ERR_UNAVAILABLE
        }

        override fun supportsFeature(featureTag: String?): Boolean {
            return mParentHost?.supportsFeature(featureTag) ?: false
        }

        override fun onEditorWorkspaceSelected(workspace: String?) {
            super.onEditorWorkspaceSelected(workspace)
            mParentHost?.onEditorWorkspaceSelected(workspace)
        }
    }

    /** 生命周期观察者 */
    private val mObserver: DefaultLifecycleObserver = object : DefaultLifecycleObserver {

        override fun onCreate(owner: LifecycleOwner) {
            super.onCreate(owner = owner)
            mHost.godot.onCreate(primaryHost = mHost)
            if (mHost.godot.onInitNativeLayer(host = mHost)) {
                mHost.godot.onInitRenderView(
                    host = mHost,
                    providedContainerLayout = mGodotContainer,
                )
            }
        }

        override fun onStart(owner: LifecycleOwner) {
            super.onStart(owner = owner)
            mHost.godot.onStart(host = mHost)
        }

        override fun onResume(owner: LifecycleOwner) {
            super.onResume(owner = owner)
            mHost.godot.onResume(host = mHost)
        }

        override fun onPause(owner: LifecycleOwner) {
            super.onPause(owner = owner)
            mHost.godot.onPause(host = mHost)
        }

        override fun onStop(owner: LifecycleOwner) {
            super.onStop(owner = owner)
            mHost.godot.onStop(host = mHost)
        }

        override fun onDestroy(owner: LifecycleOwner) {
            super.onDestroy(owner = owner)
            mHost.godot.onDestroy(primaryHost = mHost)
        }
    }

    /** Godot 插件 */
    private val mGodotPlugin: GodotPlugin by lazy {
        return@lazy object : GodotPlugin(mGodot) {

            /**
             * 插件名称
             */
            override fun getPluginName(): String {
                return PLUGIN_NAME
            }

            /**
             * 插件信号
             */
            override fun getPluginSignals(): MutableSet<SignalInfo> {
                return mutableSetOf(mSignalInfo)
            }

            /**
             * 暴露给 Godot 的接口, 发送数据到 Flutter.
             *
             * @param data 数据
             */
            @Keep
            @UsedByGodot
            fun sendData(data: String): Unit = runOnUiThread {
                if (mListeningState.get()) {
                    try {
                        mEventSink?.success(
                            mapOf(
                                "type" to "takeString",
                                "data" to data,
                            )
                        )
                    } catch (e: Exception) {
                        mEventSink?.error(
                            "GODOT_EVENT_ERROR",
                            e.message,
                            null,
                        )
                    }
                }
            }
        }
    }

    /**
     * Extract a Flutter asset from the APK to the app's cache directory
     * so Godot can access it via a real filesystem path.
     */
    private fun extractFlutterAsset(context: Context, flutterAssetKey: String): String? {
        val lookupKey = mFlutterAssets?.getAssetFilePathByName(flutterAssetKey)
            ?: "flutter_assets/$flutterAssetKey"

        val cacheDir = File(context.cacheDir, "flutter_godot")
        cacheDir.mkdirs()

        val fileName = flutterAssetKey.substringAfterLast("/")
        val targetFile = File(cacheDir, fileName)

        return try {
            context.assets.open(lookupKey).use { input ->
                targetFile.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
            Log.d(TAG, "Extracted Flutter asset '$flutterAssetKey' to: ${targetFile.absolutePath}")
            targetFile.absolutePath
        } catch (e: Exception) {
            Log.e(TAG, "Failed to extract Flutter asset '$flutterAssetKey' (lookup: $lookupKey)", e)
            null
        }
    }

    /** 私有伴生对象 */
    private companion object {
        /** 日志标签 */
        const val TAG = "FlutterGodotPlugin"

        /** 视图ID */
        const val GODOT_VIEW_ID: String = "godot-player"

        /** 方法通道ID */
        const val GODOT_METHOD_ID: String = "flutter_godot_method"

        /** 事件通道ID */
        const val GODOT_EVENT_ID: String = "flutter_godot_event"

        /** 资产名称键 */
        const val ASSET_NAME_KEY = "asset_name"

        const val EXTRA_COMMAND_LINE_PARAMS = "command_line_params"
        const val DEFAULT_WINDOW_ID = 664
        const val KEY_SHOW_GODOT_VIEW = "SHOW_GODOT_VIEW"

        /** Godot 插件名称 */
        const val PLUGIN_NAME = "flutter_godot"

        /** Godot 插件信号名称 */
        const val SIGNAL_NAME = "get_string"
    }
}