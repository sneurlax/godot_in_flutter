extends Control

# Console-based IPC for WebView approach
# - Godot -> Flutter: print() outputs JSON to console.log, captured by onConsoleMessage
# - Flutter -> Godot: evaluateJavascript() pushes to window._flutterMessages array,
#   Godot polls it via JavaScriptBridge.eval()
# - Falls back to file-based IPC for native Linux path

# File-based IPC paths (fallback for native mode)
var ipc_dir: String
var outgoing_dir: String  # Messages FROM Flutter
var incoming_dir: String  # Messages TO Flutter

var message_count: int = 0
var is_web: bool = false

# Timer for periodic messages
var _msg_timer: float = 0.0
const MSG_INTERVAL: float = 2.0

func _ready():
	print("[main.gd] === _ready() called - script is running ===")

	is_web = OS.has_feature("web")
	print("[main.gd] Running on web: %s" % str(is_web))

	if is_web:
		_setup_console_ipc()
	else:
		_init_file_ipc()

	# Initialize label
	update_label()
	print("[main.gd] _ready() complete")

func _setup_console_ipc():
	"""Initialize JavaScript-side message queue for console IPC"""
	JavaScriptBridge.eval("""
		window._flutterMessages = window._flutterMessages || [];
		window._godotReady = true;
		console.log('__GODOT_IPC__:' + JSON.stringify({"type":"ready","payload":"Godot engine initialized"}));
	""", true)
	print("[main.gd] Console IPC initialized - JS message queue ready")

func _process(_delta):
	if is_web:
		_poll_flutter_messages_web()

func _physics_process(delta):
	_msg_timer += delta
	if _msg_timer >= MSG_INTERVAL:
		_msg_timer = 0.0
		send_periodic_message()

func _poll_flutter_messages_web():
	"""Poll for messages from Flutter via JavaScript message queue"""
	var result = JavaScriptBridge.eval("""
		(function() {
			if (!window._flutterMessages || window._flutterMessages.length === 0) return '';
			var msgs = JSON.stringify(window._flutterMessages);
			window._flutterMessages = [];
			return msgs;
		})();
	""", true)

	if result != null and result is String and result != "" and result != "null":
		var parsed = JSON.parse_string(result)
		if parsed != null and parsed is Array:
			for msg in parsed:
				if msg is Dictionary:
					_handle_flutter_msg(msg)
				elif msg is String:
					var parsed_msg = JSON.parse_string(msg)
					if parsed_msg != null and parsed_msg is Dictionary:
						_handle_flutter_msg(parsed_msg)
					else:
						_handle_flutter_msg({"type": "data", "payload": msg})

func _handle_flutter_msg(msg: Dictionary):
	var msg_type = msg.get("type", "data")
	var payload = msg.get("payload", str(msg))

	if msg_type == "data":
		on_flutter_message(payload)
	elif msg_type == "input_event":
		var x = msg.get("x", 0.0)
		var y = msg.get("y", 0.0)
		var event_type = msg.get("event_type", "move")
		print("[main.gd] Input event: %s at (%s, %s)" % [event_type, str(x), str(y)])

func send_periodic_message():
	var timestamp = Time.get_ticks_msec()
	var message = "Hello from Godot: %d" % timestamp
	send_to_flutter(message)

func send_to_flutter(data: String):
	"""Send a message to Flutter via console IPC or file IPC"""
	var timestamp = Time.get_ticks_msec()
	var message_json = {
		"type": "data",
		"payload": data,
		"timestamp": timestamp
	}
	var json_string = JSON.stringify(message_json)

	if is_web:
		# Print with __GODOT_IPC__ prefix - captured by onConsoleMessage in webview_cef
		print("__GODOT_IPC__:" + json_string)
	else:
		_send_to_flutter_file(json_string, data)

func on_flutter_message(data):
	message_count += 1
	print("[main.gd] Received from Flutter #%d: %s" % [message_count, str(data)])
	update_label()

func update_label():
	var label = get_node_or_null("Label")
	if label:
		var mode = "Console IPC (WebView)" if is_web else "File Protocol IPC"
		var new_text = "Messages from Flutter: %d\n\n%s\nMessages sent every 2 seconds" % [message_count, mode]
		label.text = new_text
	else:
		print("[main.gd] ERROR: Label node not found!")

# === Legacy file-based IPC (for native Linux path) ===

var poll_timer: Timer = null

func _init_file_ipc():
	var pid = OS.get_process_id()
	ipc_dir = "/tmp/flutter_godot_ipc_%d" % pid
	outgoing_dir = "%s/outgoing" % ipc_dir
	incoming_dir = "%s/incoming" % ipc_dir
	print("[main.gd] File IPC dir: %s" % ipc_dir)

	await get_tree().process_frame

	poll_timer = Timer.new()
	add_child(poll_timer)
	poll_timer.wait_time = 0.1
	poll_timer.timeout.connect(Callable(self, "poll_incoming_messages"))
	poll_timer.start()

func poll_incoming_messages():
	var outgoing_dir_handle = DirAccess.open(outgoing_dir)
	if outgoing_dir_handle == null:
		return
	outgoing_dir_handle.list_dir_begin()
	var file_name = outgoing_dir_handle.get_next()
	while file_name != "":
		if not file_name.begins_with("."):
			var file_path = "%s/%s" % [outgoing_dir, file_name]
			var file = FileAccess.open(file_path, FileAccess.READ)
			if file != null:
				var content = file.get_as_text()
				var json = JSON.parse_string(content)
				if json != null and json is Dictionary:
					_handle_flutter_msg(json)
				var dir = DirAccess.open(outgoing_dir)
				if dir != null:
					dir.remove(file_path)
		file_name = outgoing_dir_handle.get_next()

func _send_to_flutter_file(json_string: String, data: String):
	var timestamp = Time.get_ticks_msec()
	var final_file_path = "%s/msg_%d" % [incoming_dir, timestamp]
	var file = FileAccess.open(final_file_path, FileAccess.WRITE)
	if file != null:
		file.store_string(json_string)
		print("[main.gd] File sent: %s" % data)

func _exit_tree():
	if poll_timer:
		poll_timer.queue_free()
