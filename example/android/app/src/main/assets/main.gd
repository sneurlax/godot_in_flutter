extends Control

var singleton
const pluginName: StringName = "flutter_godot"
var recv_count: int = 0

func _ready():
	if Engine.has_singleton(pluginName):
		singleton = Engine.get_singleton(pluginName)
		singleton.connect("get_string", _on_flutter_data)
		print("flutter_godot plugin connected")
	else:
		print("flutter_godot plugin not found")

func _on_button_button_down():
	if singleton:
		singleton.sendData("Hello from Godot!")

func _on_flutter_data(data: String):
	recv_count += 1
	print("Godot received: " + data)
	var label = get_node_or_null("Label")
	if label:
		label.text = "Received from Flutter (#%d):\n%s" % [recv_count, data]
