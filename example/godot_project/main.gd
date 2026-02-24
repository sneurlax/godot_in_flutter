extends Control

# File-based IPC paths
var ipc_dir: String
var outgoing_dir: String  # Messages FROM Flutter
var incoming_dir: String  # Messages TO Flutter

var message_count: int = 0
var message_timer: Timer = null
var poll_timer: Timer = null

const SOCKET_PORT: int = 64001  # Not used with file protocol

func _ready():
	print("[main.gd] === _ready() called - script is running ===")

	# Initialize IPC directories
	var pid = OS.get_process_id()
	ipc_dir = "/tmp/flutter_godot_ipc_%d" % pid
	outgoing_dir = "%s/outgoing" % ipc_dir
	incoming_dir = "%s/incoming" % ipc_dir

	print("[main.gd] IPC PID: %d" % pid)
	print("[main.gd] IPC dir: %s" % ipc_dir)

	# Create directories if they don't exist
	# Note: Flutter will create these directories first, Godot just needs to verify they exist
	await get_tree().process_frame

	# Setup timer to send messages every 2 seconds
	message_timer = Timer.new()
	add_child(message_timer)
	message_timer.wait_time = 2.0
	message_timer.timeout.connect(Callable(self, "send_periodic_message"))
	message_timer.start()

	# Setup timer to poll for incoming messages every 100ms
	poll_timer = Timer.new()
	add_child(poll_timer)
	poll_timer.wait_time = 0.1
	poll_timer.timeout.connect(Callable(self, "poll_incoming_messages"))
	poll_timer.start()

	# Initialize label
	update_label()

	# Force initial label update
	await get_tree().process_frame
	print("[main.gd] _ready() complete, scene should be visible now")

func poll_incoming_messages():
	"""Poll for incoming messages from Flutter"""
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

				# Parse JSON message
				var json = JSON.parse_string(content)
				if json != null and json is Dictionary:
					if json.get("type") == "data" and json.has("payload"):
						var payload = json["payload"]
						on_flutter_message(payload)
						print("[main.gd] Received from Flutter: %s" % payload)

				# Delete the file after processing
				var dir = DirAccess.open(outgoing_dir)
				if dir != null:
					dir.remove(file_path)

		file_name = outgoing_dir_handle.get_next()

func send_periodic_message():
	"""Send a test message to Flutter every 2 seconds"""
	var timestamp = Time.get_ticks_msec()
	var message = "Hello from Godot: %d" % timestamp

	send_to_flutter(message)

func send_to_flutter(data: String):
	"""Send a message to Flutter via file IPC"""
	var timestamp = Time.get_ticks_msec()
	var message_json = {
		"type": "data",
		"payload": data,
		"timestamp": timestamp
	}
	var json_string = JSON.stringify(message_json)

	# Write to a temp file first, then rename (atomic operation)
	var temp_file_path = "%s/msg_%d_temp" % [incoming_dir, timestamp]
	var final_file_path = "%s/msg_%d" % [incoming_dir, timestamp]

	var file = FileAccess.open(temp_file_path, FileAccess.WRITE)
	if file != null:
		# Write as plain text, not binary serialized
		file.store_string(json_string)

		# Simple rename: read and write to final location
		# Note: Godot 4.5 doesn't have atomic rename, so we just write to final
		var file2 = FileAccess.open(final_file_path, FileAccess.WRITE)
		if file2 != null:
			file2.store_string(json_string)
			print("[main.gd] Sent to Flutter: %s" % data)

		# Clean up temp file
		var dir = DirAccess.open(incoming_dir)
		if dir != null:
			dir.remove(temp_file_path)

func on_flutter_message(data: String):
	"""Handle messages received from Flutter"""
	message_count += 1
	print("[main.gd] Received from Flutter #%d: %s" % [message_count, data])
	update_label()

func update_label():
	"""Update the label with current message count"""
	var label = get_node_or_null("Label")
	if label:
		var new_text = "Messages from Godot: %d\n\nFile Protocol IPC\nMessages sent every 2 seconds" % message_count
		label.text = new_text
		print("[main.gd] Updated label: %d messages" % message_count)
	else:
		print("[main.gd] ERROR: Label node not found!")

func _exit_tree():
	"""Clean up resources on exit"""
	if message_timer:
		message_timer.queue_free()
	if poll_timer:
		poll_timer.queue_free()
