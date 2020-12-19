#!/usr/bin/env python3

import sys
import os

import gi

import protos.mangohud_pb2 as pb

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, GLib, Gdk, Gio, GObject

screen = Gdk.Screen.get_default()
gtk_provider = Gtk.CssProvider()

gtk_context = Gtk.StyleContext
#gtk_context.add_provider_for_screen(screen, gtk_provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
#css = b""".bigone { font-size: 32px; font-weight: bold; }"""
#gtk_provider.load_from_data(css)  # For some reasons, it doesn't work.
css_file = Gio.File.new_for_path("gui.css")
gtk_provider.load_from_file(css_file)

gtk_context.add_provider_for_screen(screen, gtk_provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)


builder = Gtk.Builder()
builder.add_from_file("gui.glade")

# builder.add_objects_from_file("example.glade", ("button1", "button2"))

import threading
import time

connect_button = builder.get_object("connect_button")

SUPPORTED_PROTOCOL_VERSION = 1

ADDRESS = ("localhost", 9869)

# The length verification to system limits, will be checked by Python wrapper
# in `sock.connect`.
if os.getuid:
    SOCKET_NAME = f"/tmp/mangohud_server-{os.getuid()}.socket"
else:
    # For Windows.
    SOCKET_NAME = f"/tmp/mangohud_server.socket"

if "MANGOHUD_SERVER" in os.environ:
    SOCKET_NAME = os.environ["MANGOHUD_SERVER"]

# TODO(baryluk): Automatically determine type of server (UNIX vs TCP),
# from MANGOHUD_SERVER variable.

import socket

def send(sock, msg):
    serialized = msg.SerializeToString()
    serialized_size = len(serialized)
    sock.send(serialized_size.to_bytes(4, 'big'))  # Convert to network order.
    sock.send(serialized)

def recv(sock):
    header = sock.recv(4)
    size = int.from_bytes(header, 'big')  # or use socket.ntohl ?
    data = sock.recv(size)
    msg = pb.Message()
    msg.ParseFromString(data)
    return msg

thread = None

stop_ev = threading.Event()


@Gtk.Template(filename='client_template.glade')
class ClientWidget(Gtk.Grid):
    __gtype_name__ = 'ClientWidget'

    def __init__(self, *, hud_toggle_cb, **kwargs):
        super().__init__(**kwargs)
        self.hud_toggle.connect("notify::active", hud_toggle_cb)
        # self.log_toggle.connect("notify::active", log_toggle_cb)

    @Gtk.Template.Callback('hud_toggle_activate_cb')
    def on_hud_toggle_toggled(self, widget):
        print("HUD toggled")
        pass

    # @Gtk.Template.Callback('log_toggle_activate_cb')
    def on_log_toggle_toggled(self, widget):
        print("Log toggled")
        pass

    # GtkLabel
    fps = Gtk.Template.Child('fps')
    app_name = Gtk.Template.Child('app_name')
    api = Gtk.Template.Child('api')

    # GtkSwitch
    hud_toggle = Gtk.Template.Child('hud_toggle')
    log_toggle = Gtk.Template.Child('log_toggle')

class Client(object):
    def __init__(self, *, last_msg_state, row_index):
        # Instance of ClientWidget with updated stuff.
        self.last_msg_state = last_msg_state
        self.nodename = last_msg_state.nodename
        self.pid = last_msg_state.pid
        self.hud_change_requested = False
        self.hud_change_request = False
        self.hud_change_requested_at_msg_counter = float('-inf')
        self.msg_counter = 0

        # Create Gtk client widget instance
        self.client_widget = ClientWidget(hud_toggle_cb=self.hud_toggle_notify_active_cb)

        # row in the parent GtkGrid that the client_widget is in
        self.row_index = row_index

    def hud_toggle_notify_active_cb(self, toggle_widget : 'GtkSwitch', property):
        # TODO(baryluk): Wire things together.
        self.hud_change_requested = True
        self.hud_change_request = toggle_widget.get_active()
        self.hud_change_requested_at_msg_counter = self.msg_counter

        # self.notify(...)
        pass

# The key is (nodename, pid) => ClientWidget
known_clients = {}

clients_container = builder.get_object('clients_container')

last_row_count = 1  # Actually past the last one.

def handle_message(msg):
    global known_clients
    global last_row_count

    if msg.clients:
        new_clients = False
        for client_msg in msg.clients:
            key = (client_msg.nodename, client_msg.pid)
            if key not in known_clients:
                new_clients = True
                client = Client(last_msg_state=client_msg, row_index=last_row_count)
                known_clients[key] = client

                # This probably is not safe to do from this thread
                # clients_container.insert_next_to(last_row, Gtk.PositionType.BOTTOM)
                clients_container.attach(client.client_widget, left=0, top=client.row_index, width=1, height=1)
                # last_row = client.client_widget
                last_row_count += 1
            else:
                known_clients[key].last_msg_state = client_msg
            known_clients[key].msg_counter += 1
        if new_clients:
            clients_container.show_all()

        # TODO(baryluk): Remove stale clients or once we know for
        # sure are down.

        for key, client in known_clients.items():
            client_msg = client.last_msg_state
            client_widget = client.client_widget
            client_widget.fps.set_text(f"{client_msg.fps:.0f}")
            client_widget.app_name.set_text(f"{client_msg.program_name}")
            api = ""
            if client_msg.render_info and client_msg.render_info.opengl:
                opengl = "OpenGL ES" if client_msg.render_info.opengl_is_gles else "OpenGL"
                api = f"{opengl} {client_msg.render_info.opengl_version_major}.{client_msg.render_info.opengl_version_minor}"
            elif client_msg.render_info and client_msg.render_info.vulkan:
                api = f"Vulkan {client_msg.render_info.vulkan_version_major}.{client_msg.render_info.vulkan_version_minor}.{client_msg.render_info.vulkan_version_patch}"
            else:
                api = f"pid {client_msg.pid}"
            if client_msg.render_info.gpu_name:
                api += f"\n{client_msg.render_info.gpu_name}"
            if client_msg.render_info.driver_name:
                api += f"\n{client_msg.render_info.driver_name}"
            if client_msg.render_info.engine_name:
                api += f"\n{client_msg.render_info.engine_name}"
            if client_msg.wine_version:
                api += f"\nWine {client_msg.wine_version}"
            client_widget.api.set_text(api)
            # Add time details in tooltip (like last update).
            tooltip = f"pid {client_msg.pid} @ {client_msg.nodename}; uid {client_msg.uid}; username {client_msg.username}"
            client_widget.api.set_tooltip_text(tooltip)

            # TODO(baryluk): If user toggled it On, but next few msg, still show client.show_hud
            # False, don't immedietly toggle it back.
            # This works, but is hacky.
            # Maybe get some sequence numbers from client or something.
            if client.msg_counter > client.hud_change_requested_at_msg_counter + 20:
                client_widget.hud_toggle.set_active(client_msg.show_hud)
            client_widget.hud_toggle.set_sensitive(True)

            # TODO(baryluk): Garbage collect old clients.


def thread_loop(sock):
    protocol_version_warning_shown = False
    while not stop_ev.is_set():
        msg = pb.Message(protocol_version=1, client_type=pb.Message.ClientType.GUI)

        # Not thread safe, as known_clients is updated by main thread.
        # And the client.hud_change_request might be update by main thread too.
        for client_key, client in known_clients.items():
            if client.hud_change_requested:
                last_msg = client.last_msg_state
                client_msg = pb.Message()
                # Populate fields that the server can identify which client we
                # are talking about.
                client_msg.nodename = last_msg.nodename
                client_msg.uid = last_msg.uid
                client_msg.pid = last_msg.pid
                # Set desired state.
                client_msg.change_show_hud = True  # Workaround lack of optional in proto3.
                client_msg.show_hud = client.hud_change_request
                # Reset the request.
                client.hud_change_requested = False
                client.hud_change_request = None
                msg.clients.append(client_msg)

        # print("Request:", msg)
        send(sock, msg)

        msg = recv(sock)
        # print("Response:", msg)
        # print()

        if (msg.protocol_version and msg.protocol_version > SUPPORTED_PROTOCOL_VERSION):
            if not protocol_version_warnings_shown:
                print(f"Warning: Server speaks newer protocol_version {msg.protocol_version}, than supported by this app ({SUPPORTED_PROTOCOL_VERSION}).\nCrashes or missing functionality expected!\nPlease upgrade!");
                protocol_version_warnings_shown = True

        # Process message in the main thread, as we will be updating GUI
        # while we process it.
        GLib.idle_add(handle_message, msg)

        # Sleep less if 50ms laready passed from previous contact.
        if len(msg.clients) == 0:
          time.sleep(1.00)
        else:
          time.sleep(0.25)

def thread_loop_start(sock):
    print("Connected")
    # TODO(baryluk): Show "Connected" first, then after few seconds fade to "Disconnect".
    GLib.idle_add(connect_button.set_label, "Disconnect")
    thread_loop(sock)

def connection_thread():
    global thread, stop_ev
    status = "Connecting"
    reconnect = True
    reconnect_delay = 1.0
    while reconnect and not stop_ev.is_set():
        try:
            if True:
                addresses = socket.getaddrinfo(ADDRESS[0], ADDRESS[1], proto=socket.IPPROTO_TCP)
                assert addresses
                address = addresses[0]  # (family, type, proto, canonname, sockaddr)
                family, type_, proto, canonname, sockaddr = address
                assert type_ == socket.SOCK_STREAM
                print(f"Connecting to {address}")
                with socket.socket(family=family, type=socket.SOCK_STREAM | socket.SOCK_CLOEXEC, proto=proto) as sock:
                    sock.connect(sockaddr)
                    # TODO(baryluk): This is too simplistic. It is still
                    # possible to connect, yet be disconnected after parsing the
                    # first message, and do reconnecting in fast loop.
                    # Improve fallback, i.e. only reset it to initial value,
                    # if few second passed and at least few messages were
                    # exchanged.
                    reconnect_delay = 1.0
                    thread_loop_start(sock)
                sock.close()
            else:
                print(f"Connecting to {SOCKET_NAME}")
                with socket.socket(family=socket.AF_UNIX, type=socket.SOCK_STREAM | socket.SOCK_CLOEXEC) as sock:
                    sock.connect(SOCKET_NAME)
                    reconnect_delay = 1.0  # See comment above for TCP.
                    thread_loop_start(sock)
                sock.close()
            status = ""
        except BrokenPipeError as e:
            status = "Broken pipe to server"
        except ConnectionRefusedError as e:
            status = "Connection refused to server (is it down?)"
        except NameError as e:
            print("Internal error")
            status = "Error"
            stop = True
            stop_ev.set()
            # raise e  # Some code bug.
            GLib.idle_add(connect_button.set_label, "Error")
            raise e
        finally:
            print(f"Disconnected: status = {status}")
            if not stop_ev.is_set():
                reconnect_time = time.time() + reconnect_delay
                while time.time() < reconnect_time:
                    reconnect_togo = max(0.0, reconnect_time - time.time())
                    GLib.idle_add(connect_button.set_label, f"Reconnecting in {reconnect_togo:.1f}s")
                    time.sleep(0.1)
                reconnect_delay = min(30.0, reconnect_delay * 1.4)
                GLib.idle_add(connect_button.set_label, "Reconnecting...")
            else:
                if status == "Error":
                    GLib.idle_add(connect_button.set_label, "Error!")
                    # raise NameError()
                else:
                    GLib.idle_add(connect_button.set_label, "Connect")
    thread = None
    stop_ev.clear()

def connect_clicked(button):
    global thread
    if connect_button.get_label() == "Disconnect":
        stop_ev.set()
        return

    if thread:
        print("Already connected or connect in progress")
        # TODO: If explicitly clicked while we are in "Reconnecting in {...}"
        # phase. Force reconnect.
        return

    print("Connecting...")
    # button.label.set_text("Connecting...")
    GLib.idle_add(connect_button.set_label, "Connecting...")
    thread = threading.Thread(target=connection_thread)
    # thread.daemon = True  # This means to not wait for the thread on exit. Just kill it.
    thread.start()

def clear_clicked(button):
    global last_row_count
    global known_clients
    global clients_container

    rows_to_remove = []
    # That is pretty strong way of doing things.
    for client_key, client in known_clients.items():
        rows_to_remove.append(client.row_index)
    # GtkGrid.remove_row removes the row and child in it,
    # but also moves all the rows below up by one.
    # So remove from the end.
    for row_index in sorted(rows_to_remove, reverse=True):
        clients_container.remove_row(row_index)
    # Pretty heavy.
    known_clients.clear()
    last_row_count = 1
    # for client_key, client in known_clients.items():
    pass

handlers = {
    "connect_clicked": connect_clicked,
    "clear_button_clicked_cb": clear_clicked,
}

builder.connect_signals(handlers)

window = builder.get_object("window_main")

#header_bar = builder.get_object("headerbar1")
#window.set_titlebar(header_bar)

window.connect("destroy", Gtk.main_quit)

# record_toggle_button = builder.get_object("record_toggle")
# clear_button = builder.get_object("clear_button")
# preferences_button = builder.get_object("preferences_button")


try:
    window.show_all()

    # Auto connect on startup.
    connect_clicked(connect_button)

    Gtk.main()
finally:
    stop_ev.set()
