# Multithreaded Chat Application

This project is a multithreaded chat application written in C, featuring a GTK-based graphical user interface for the client and a custom circular message queue on the server. 

---

---

##  Project Structure

```
MULTITHREADED-CHAT/
├── chat_client.c        # Client implementation with GTK UI
├── chat_client          # Compiled client binary
├── chat_server.c        # Server implementation
├── chat_server.h
├── chat_server          # Compiled server binary
├── circular_queue.c     # Circular queue (PE1)
├── circular_queue.h
├── udp.h                # Networking utilities
├── client               # Optional client launcher
├── server               # Optional server launcher
└── iChat.txt            # Test logs / sample chat log


---

## 🛠️ Compilation Instructions

### **Client (GTK UI)**

Make sure you have GTK 3 installed. Then compile using:

```bash
gcc chat_client.c -lpthread $(pkg-config --cflags --libs gtk+-3.0) -o client
```

### **Server**

```bash
gcc chat_server.c circular_queue.c -lpthread -o server
```

---

## 🔄 Circular Queue (PE1)

As part of Proposed Extension 1, a circular queue was implemented to store the most recent 15 messages on the server side.

### Behavior:

Client → Server Commands

| Command Format              | Description                                        |
| --------------------------- | -------------------------------------------------- |
| `conn$ client_name`         | Connect to the server with the given username      |
| `say$ msg`                  | Broadcast a message to all users                   |
| `sayto$ recipient_name msg` | Send a private message to a specific user          |
| `mute$ client_name`         | Mute a user (their messages won’t be shown to you) |
| `unmute$ client_name`       | Unmute a previously muted user                     |
| `rename$ new_name`          | Change your username                               |
| `disconn$`                  | Disconnect from the server                         |
| `kick$ client_name`         | **Admin only:** force a user to disconnect         |


Circular queue 
* `head` points to the **oldest** message.
* `tail` points to the **insertion index (newest + 1)**.
* Once full, inserting a new message **overwrites the oldest one** by moving `head` forward.
* Ensures efficient storage and constant-time enqueue operations.

This allows new clients to receive recent chat history upon joining.

---

## ▶️ Running the Application

### Start the Server:

```bash
./server
```

### Start the Client:

```bash
./client
```

Multiple clients can be opened simultaneously.

---

## 📦 Dependencies

* **C (GCC)**
* **POSIX threads (pthread)**
* **GTK+ 3.0** for the client UI
* **pkg-config** for GTK build flags

---
