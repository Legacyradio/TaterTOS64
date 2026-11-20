#!/usr/bin/env python3
"""Fast stdio MCP agent-bus for Tater Bridge validation.

The normal host server imports FastMCP/anyio/pydantic before it can answer the
MCP initialize request. Under TaterTOS's current Linux filesystem path that cold
start exceeds Claude Code's MCP timeout. This server speaks the small MCP
JSON-RPC surface directly and imports sqlite lazily, after handshake.
"""

import json
import os
import sys
import time

MESSAGE_TYPES = {"task", "question", "result", "status", "blocker"}
TASK_STATES = {"open", "claimed", "done"}


def _db_path():
    return os.environ.get("AGENT_BUS_DB") or os.path.join(
        os.environ.get("HOME", "/nvme"), ".agent-bus", "bus.db"
    )


def _conn():
    import sqlite3

    path = _db_path()
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    conn = sqlite3.connect(path, timeout=30.0)
    conn.row_factory = sqlite3.Row
    try:
        conn.execute("PRAGMA journal_mode=WAL")
    except Exception:
        pass
    conn.execute("PRAGMA busy_timeout=30000")
    conn.execute("PRAGMA foreign_keys=ON")
    _init_db(conn)
    return conn


def _init_db(conn):
    conn.executescript(
        """
        CREATE TABLE IF NOT EXISTS agents (
            name       TEXT PRIMARY KEY,
            role       TEXT,
            last_seen  REAL
        );
        CREATE TABLE IF NOT EXISTS messages (
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            sender    TEXT NOT NULL,
            recipient TEXT NOT NULL,
            type      TEXT NOT NULL,
            subject   TEXT,
            body      TEXT,
            created   REAL NOT NULL,
            read_at   REAL
        );
        CREATE TABLE IF NOT EXISTS tasks (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            title      TEXT NOT NULL,
            detail     TEXT,
            status     TEXT NOT NULL DEFAULT 'open',
            created_by TEXT,
            claimed_by TEXT,
            result     TEXT,
            created    REAL NOT NULL,
            updated    REAL NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_msg_recipient ON messages(recipient, read_at);
        CREATE INDEX IF NOT EXISTS idx_task_status ON tasks(status);
        """
    )


def _now():
    return time.time()


def _touch(conn, agent):
    conn.execute(
        "INSERT INTO agents(name, role, last_seen) VALUES(?, NULL, ?) "
        "ON CONFLICT(name) DO UPDATE SET last_seen=excluded.last_seen",
        (agent, _now()),
    )


def _fmt_msg(row):
    return {
        "id": row["id"],
        "from": row["sender"],
        "to": row["recipient"],
        "type": row["type"],
        "subject": row["subject"],
        "body": row["body"],
        "created": row["created"],
        "read": row["read_at"] is not None,
    }


def _fmt_task(row):
    return {
        "id": row["id"],
        "title": row["title"],
        "detail": row["detail"],
        "status": row["status"],
        "created_by": row["created_by"],
        "claimed_by": row["claimed_by"],
        "result": row["result"],
    }


def bus_register(agent, role=""):
    with _conn() as conn:
        conn.execute(
            "INSERT INTO agents(name, role, last_seen) VALUES(?,?,?) "
            "ON CONFLICT(name) DO UPDATE SET role=excluded.role, last_seen=excluded.last_seen",
            (agent, role, _now()),
        )
        peers = [
            {"name": r["name"], "role": r["role"]}
            for r in conn.execute("SELECT name, role FROM agents WHERE name != ?", (agent,))
        ]
    return "registered '%s' (role: %s). peers: %s" % (
        agent,
        role or "unspecified",
        peers or "none yet",
    )


def bus_send(sender, recipient, type, subject, body=""):
    if type not in MESSAGE_TYPES:
        return "error: type must be one of %s" % sorted(MESSAGE_TYPES)
    with _conn() as conn:
        _touch(conn, sender)
        cur = conn.execute(
            "INSERT INTO messages(sender, recipient, type, subject, body, created) "
            "VALUES(?,?,?,?,?,?)",
            (sender, recipient, type, subject, body, _now()),
        )
        mid = cur.lastrowid
    return "sent message #%s (%s) to %s" % (mid, type, recipient)


def bus_inbox(agent, unread_only=True, limit=50):
    with _conn() as conn:
        _touch(conn, agent)
        q = (
            "SELECT * FROM messages WHERE (recipient=? OR recipient='broadcast') "
            "AND sender != ?"
        )
        params = [agent, agent]
        if unread_only:
            q += " AND read_at IS NULL"
        q += " ORDER BY id ASC LIMIT ?"
        params.append(limit)
        rows = conn.execute(q, params).fetchall()
        ids = [r["id"] for r in rows]
        if ids:
            conn.execute(
                "UPDATE messages SET read_at=? WHERE id IN (%s)" % ",".join("?" * len(ids)),
                [_now()] + ids,
            )
    return [_fmt_msg(r) for r in rows]


def bus_ack(message_id):
    with _conn() as conn:
        conn.execute("UPDATE messages SET read_at=? WHERE id=?", (_now(), message_id))
    return "acked #%s" % message_id


def bus_post_task(created_by, title, detail=""):
    with _conn() as conn:
        _touch(conn, created_by)
        cur = conn.execute(
            "INSERT INTO tasks(title, detail, status, created_by, created, updated) "
            "VALUES(?,?, 'open', ?, ?, ?)",
            (title, detail, created_by, _now(), _now()),
        )
        tid = cur.lastrowid
    return "posted task #%s: %s" % (tid, title)


def bus_list_tasks(status="", limit=50):
    with _conn() as conn:
        if status:
            rows = conn.execute(
                "SELECT * FROM tasks WHERE status=? ORDER BY id ASC LIMIT ?",
                (status, limit),
            ).fetchall()
        else:
            rows = conn.execute("SELECT * FROM tasks ORDER BY id ASC LIMIT ?", (limit,)).fetchall()
    return [_fmt_task(r) for r in rows]


def bus_claim_task(agent, task_id):
    with _conn() as conn:
        _touch(conn, agent)
        row = conn.execute("SELECT * FROM tasks WHERE id=?", (task_id,)).fetchone()
        if row is None:
            return "error: task #%s not found" % task_id
        if row["status"] != "open":
            return "error: task #%s is '%s' (claimed_by=%s)" % (
                task_id,
                row["status"],
                row["claimed_by"],
            )
        conn.execute(
            "UPDATE tasks SET status='claimed', claimed_by=?, updated=? WHERE id=?",
            (agent, _now(), task_id),
        )
    return "claimed task #%s" % task_id


def bus_complete_task(agent, task_id, result):
    with _conn() as conn:
        _touch(conn, agent)
        row = conn.execute("SELECT * FROM tasks WHERE id=?", (task_id,)).fetchone()
        if row is None:
            return "error: task #%s not found" % task_id
        conn.execute(
            "UPDATE tasks SET status='done', claimed_by=?, result=?, updated=? WHERE id=?",
            (agent, result, _now(), task_id),
        )
    return "completed task #%s" % task_id


def bus_wait(agent, want_type="", from_agent="", timeout_s=60):
    deadline = _now() + max(0, min(int(timeout_s), 240))
    while True:
        with _conn() as conn:
            _touch(conn, agent)
            q = (
                "SELECT * FROM messages WHERE (recipient=? OR recipient='broadcast') "
                "AND sender != ? AND read_at IS NULL"
            )
            params = [agent, agent]
            if want_type:
                q += " AND type=?"
                params.append(want_type)
            if from_agent:
                q += " AND sender=?"
                params.append(from_agent)
            q += " ORDER BY id ASC LIMIT 1"
            row = conn.execute(q, params).fetchone()
            if row:
                conn.execute("UPDATE messages SET read_at=? WHERE id=?", (_now(), row["id"]))
                return _fmt_msg(row)
        if _now() >= deadline:
            return {"timeout": True}
        time.sleep(1)


def _schema(props, required):
    return {"type": "object", "properties": props, "required": required}


TOOLS = {
    "bus_register": (
        bus_register,
        "Register/identify yourself on the bus.",
        _schema({"agent": {"type": "string"}, "role": {"type": "string", "default": ""}}, ["agent"]),
    ),
    "bus_send": (
        bus_send,
        "Send a message to another agent or broadcast.",
        _schema(
            {
                "sender": {"type": "string"},
                "recipient": {"type": "string"},
                "type": {"type": "string", "enum": sorted(MESSAGE_TYPES)},
                "subject": {"type": "string"},
                "body": {"type": "string", "default": ""},
            },
            ["sender", "recipient", "type", "subject"],
        ),
    ),
    "bus_inbox": (
        bus_inbox,
        "Read messages addressed to an agent.",
        _schema(
            {
                "agent": {"type": "string"},
                "unread_only": {"type": "boolean", "default": True},
                "limit": {"type": "integer", "default": 50},
            },
            ["agent"],
        ),
    ),
    "bus_ack": (
        bus_ack,
        "Mark one message read.",
        _schema({"message_id": {"type": "integer"}}, ["message_id"]),
    ),
    "bus_post_task": (
        bus_post_task,
        "Post a unit of work to the task queue.",
        _schema(
            {
                "created_by": {"type": "string"},
                "title": {"type": "string"},
                "detail": {"type": "string", "default": ""},
            },
            ["created_by", "title"],
        ),
    ),
    "bus_list_tasks": (
        bus_list_tasks,
        "List tasks, optionally filtered by status.",
        _schema(
            {"status": {"type": "string", "default": ""}, "limit": {"type": "integer", "default": 50}},
            [],
        ),
    ),
    "bus_claim_task": (
        bus_claim_task,
        "Claim an open task.",
        _schema({"agent": {"type": "string"}, "task_id": {"type": "integer"}}, ["agent", "task_id"]),
    ),
    "bus_complete_task": (
        bus_complete_task,
        "Complete a claimed task.",
        _schema(
            {
                "agent": {"type": "string"},
                "task_id": {"type": "integer"},
                "result": {"type": "string"},
            },
            ["agent", "task_id", "result"],
        ),
    ),
    "bus_wait": (
        bus_wait,
        "Wait until a matching unread message arrives.",
        _schema(
            {
                "agent": {"type": "string"},
                "want_type": {"type": "string", "default": ""},
                "from_agent": {"type": "string", "default": ""},
                "timeout_s": {"type": "integer", "default": 60},
            },
            ["agent"],
        ),
    ),
}


def _tool_defs():
    return [
        {"name": name, "description": desc, "inputSchema": schema}
        for name, (_fn, desc, schema) in TOOLS.items()
    ]


def _send(obj, mode):
    data = json.dumps(obj, separators=(",", ":")).encode("utf-8")
    if mode == "content-length":
        sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(data))
        sys.stdout.buffer.write(data)
    else:
        sys.stdout.buffer.write(data + b"\n")
    sys.stdout.buffer.flush()


def _result(req_id, result, mode):
    _send({"jsonrpc": "2.0", "id": req_id, "result": result}, mode)


def _error(req_id, code, message, mode):
    _send({"jsonrpc": "2.0", "id": req_id, "error": {"code": code, "message": message}}, mode)


def _call_tool(req_id, params, mode):
    name = (params or {}).get("name", "")
    args = (params or {}).get("arguments") or {}
    entry = TOOLS.get(name)
    if not entry:
        _error(req_id, -32601, "unknown tool: %s" % name, mode)
        return
    fn = entry[0]
    try:
        value = fn(**args)
        text = value if isinstance(value, str) else json.dumps(value, separators=(",", ":"))
        _result(
            req_id,
            {
                "content": [{"type": "text", "text": text}],
                "structuredContent": {"result": value},
                "isError": False,
            },
            mode,
        )
    except Exception as exc:
        _result(
            req_id,
            {
                "content": [{"type": "text", "text": "error: %s" % exc}],
                "isError": True,
            },
            mode,
        )


def _read_one():
    first = sys.stdin.buffer.readline()
    if not first:
        return None, None

    stripped = first.lstrip()
    if stripped.startswith(b"{"):
        return json.loads(first.decode("utf-8")), "line"

    headers = [first]
    while first not in (b"\n", b"\r\n", b""):
        first = sys.stdin.buffer.readline()
        headers.append(first)
    length = None
    for header in headers:
        if header.lower().startswith(b"content-length:"):
            length = int(header.split(b":", 1)[1].strip())
            break
    if length is None:
        return None, "content-length"
    body = sys.stdin.buffer.read(length)
    if len(body) != length:
        return None, "content-length"
    return json.loads(body.decode("utf-8")), "content-length"


def main():
    while True:
        try:
            msg, mode = _read_one()
        except Exception:
            continue
        if msg is None:
            break
        req_id = msg.get("id")
        method = msg.get("method")
        params = msg.get("params") or {}
        if req_id is None:
            continue
        if method == "initialize":
            proto = params.get("protocolVersion", "2025-06-18")
            _result(
                req_id,
                {
                    "protocolVersion": proto,
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "agent-bus", "version": "tbridge-fast"},
                },
                mode,
            )
        elif method == "ping":
            _result(req_id, {}, mode)
        elif method == "tools/list":
            _result(req_id, {"tools": _tool_defs()}, mode)
        elif method == "tools/call":
            _call_tool(req_id, params, mode)
        elif method == "resources/list":
            _result(req_id, {"resources": []}, mode)
        elif method == "prompts/list":
            _result(req_id, {"prompts": []}, mode)
        else:
            _error(req_id, -32601, "method not found: %s" % method, mode)


if __name__ == "__main__":
    main()
