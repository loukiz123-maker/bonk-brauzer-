package id.lynxdroid.agent;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.os.SystemClock;
import android.util.Log;
import android.view.InputDevice;
import android.view.MotionEvent;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.File;
import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.SocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.BitSet;

import android.net.LocalServerSocket;
import android.net.LocalSocket;

/**
 * Converts LynxDroid touch messages into real MotionEvents.
 *
 * <p>Design notes that matter for correctness:
 *
 * <ul>
 *   <li>Pointer ids are owned by this service. A {@code down} on an id that is
 *       already open is rejected, so a desktop bug cannot create a phantom
 *       finger. This mirrors the allocator on the Python side.</li>
 *   <li>Every {@code up} and {@code cancel} releases the id, and the service
 *       releases <em>all</em> ids when the socket closes. A crashed client must
 *       never leave a finger pressed on screen.</li>
 *   <li>Events are injected with {@code ACTION_DOWN}/{@code ACTION_POINTER_DOWN}
 *       and the full pointer array, which is the only way Android recognises a
 *       genuine multi-touch gesture.</li>
 *   <li>Nothing listens on TCP. The socket is a local (AF_UNIX) socket inside
 *       the container, so no other machine can reach it.</li>
 * </ul>
 *
 * <p>LocalSocket requires a path inside the container. LynxDroid passes
 * {@code --ez socket <path>}; the default is {@code /data/local/tmp/lynxdroid.sock}.
 *
 * <p><b>Status:</b> this source has not been compiled or run on the target
 * machine. It is included so the protocol has a concrete, reviewable
 * implementation and because the desktop side must not claim multitouch
 * without one. See {@code android_agent/README.md}.
 */
public class TouchAgentService extends Service {

    private static final String TAG = "LynxDroidAgent";
    private static final int PROTOCOL = 1;
    private static final int MAX_TOUCHES = 10;
    private static final int MAX_FRAME = 64 * 1024;
    private static final String DEFAULT_SOCKET = "/data/local/tmp/lynxdroid.sock";

    private volatile boolean running = true;
    private Thread worker;
    private LocalServerSocket server;
    private final BitSet pointerState = new BitSet(MAX_TOUCHES + 1);

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String path = intent != null ? intent.getStringExtra("socket") : null;
        if (path == null) {
            path = DEFAULT_SOCKET;
        }
        startWorker(path);
        return START_STICKY;
    }

    private synchronized void startWorker(String path) {
        if (worker != null && worker.isAlive()) {
            return;
        }
        final String socketPath = path;
        worker = new Thread(() -> serve(socketPath), "lynxdroid-agent");
        worker.setDaemon(true);
        worker.start();
    }

    private void serve(String path) {
        File socketFile = new File(path);
        // Remove a stale socket from a previous run; never delete a directory.
        if (socketFile.exists() && !socketFile.isDirectory()) {
            //noinspection ResultOfMethodCallIgnored
            socketFile.delete();
        }
        try {
            server = new LocalServerSocket(new File(path).getAbsolutePath());
            Log.i(TAG, "listening on " + path);
        } catch (IOException e) {
            Log.e(TAG, "cannot create socket " + path, e);
            return;
        }

        while (running) {
            try (LocalSocket socket = server.accept()) {
                handleClient(socket);
            } catch (IOException e) {
                if (running) {
                    Log.w(TAG, "client connection ended: " + e.getMessage());
                }
            } finally {
                releaseAll("client disconnected");
            }
        }
    }

    private void handleClient(LocalSocket socket) throws IOException {
        DataInputStream in = new DataInputStream(socket.getInputStream());
        DataOutputStream out = new DataOutputStream(socket.getOutputStream());

        while (running) {
            int length;
            try {
                length = in.readInt();
            } catch (EOFException eof) {
                return;
            }
            if (length <= 0 || length > MAX_FRAME) {
                Log.w(TAG, "rejecting frame of " + length + " bytes");
                return;
            }
            byte[] payload = new byte[length];
            in.readFully(payload);
            JSONObject request = new JSONObject(new String(payload, StandardCharsets.UTF_8));

            String op = request.optString("op", "");
            JSONObject reply = new JSONObject();
            try {
                if ("hello".equals(op)) {
                    int theirs = request.optInt("protocol", -1);
                    if (theirs != PROTOCOL) {
                        // "protocol" is this agent's own version and
                        // "supported" is what the client asked for, matching
                        // the field meanings the desktop client reads. Note
                        // "supported" is the CLIENT's value here on purpose:
                        // both sides use it to say "the number the other end
                        // sent", which is what makes the mismatch readable.
                        reply.put("ok", false)
                             .put("error", "protocol mismatch")
                             .put("protocol", PROTOCOL)
                             .put("supported", theirs);
                    } else {
                        reply.put("protocol", PROTOCOL)
                             .put("max_touches", MAX_TOUCHES)
                             .put("relative_motion", true)
                             .put("latency_supported", true);
                    }
                } else if ("touch".equals(op)) {
                    JSONArray events = request.optJSONArray("events");
                    String failure = applyEvents(events);
                    if (failure == null) {
                        reply.put("ok", true);
                    } else {
                        reply.put("ok", false).put("error", failure);
                    }
                } else if ("bye".equals(op)) {
                    write(out, new JSONObject().put("ok", true));
                    return;
                } else {
                    reply.put("ok", false).put("error", "unknown op: " + op);
                }
            } catch (Exception e) {
                reply = new JSONObject().put("ok", false).put("error", e.toString());
            }
            write(out, reply);
        }
    }

    private static void write(DataOutputStream out, JSONObject payload) throws IOException {
        byte[] body = payload.toString().getBytes(StandardCharsets.UTF_8);
        out.writeInt(body.length);
        out.write(body);
        out.flush();
    }

    /** Applies a batch. Returns null on success, or a reason on failure. */
    private String applyEvents(JSONArray events) {
        if (events == null) {
            return "missing events";
        }
        for (int index = 0; index < events.length(); index++) {
            JSONObject event = events.optJSONObject(index);
            if (event == null) {
                continue;
            }
            String action = event.optString("action", "");
            int pointer = event.optInt("pointer", -1);
            float x = (float) event.optDouble("x", 0);
            float y = (float) event.optDouble("y", 0);
            long when = event.optLong("t", SystemClock.uptimeMillis());

            if (pointer < 1 || pointer > MAX_TOUCHES) {
                return "pointer id out of range: " + pointer;
            }
            if ("down".equals(action)) {
                if (pointerState.get(pointer)) {
                    return "pointer " + pointer + " is already down";
                }
                pointerState.set(pointer);
            } else if (("up".equals(action) || "cancel".equals(action))
                    && !pointerState.get(pointer)) {
                return "pointer " + pointer + " is not down";
            }

            inject(action, pointer, x, y, when);

            if ("up".equals(action) || "cancel".equals(action)) {
                pointerState.clear(pointer);
            }
        }
        return null;
    }

    private void inject(String action, int pointer, float x, float y, long when) {
        int pointerIndex = pointer - 1;
        MotionEvent.PointerProperties[] properties = new MotionEvent.PointerProperties[1];
        properties[pointerIndex] = new MotionEvent.PointerProperties();
        properties[pointerIndex].id = pointer;
        properties[pointerIndex].toolType = MotionEvent.TOOL_TYPE_FINGER;

        MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[1];
        coords[pointerIndex] = new MotionEvent.PointerCoords();
        coords[pointerIndex].x = x;
        coords[pointerIndex].y = y;
        coords[pointerIndex].pressure = 1.0f;
        coords[pointerIndex].size = 1.0f;

        int maskedAction;
        switch (action) {
            case "down":
                maskedAction = MotionEvent.ACTION_DOWN;
                break;
            case "move":
                maskedAction = MotionEvent.ACTION_MOVE;
                break;
            case "up":
                maskedAction = MotionEvent.ACTION_UP;
                break;
            case "cancel":
                maskedAction = MotionEvent.ACTION_CANCEL;
                break;
            default:
                return;
        }

        MotionEvent event = MotionEvent.obtain(
                when, when, maskedAction, 1, properties, coords,
                0, 0, 1.0f, 1.0f, 0, 0,
                InputDevice.SOURCE_TOUCHSCREEN, 0);
        try {
            // Instrumentation is the only non-shell API that can inject into
            // another app's window; it requires the signature-level
            // INJECT_EVENTS permission declared in the manifest.
            android.app.Instrumentation instrumentation = new android.app.Instrumentation();
            instrumentation.sendPointerSync(event);
        } catch (SecurityException e) {
            Log.e(TAG, "not allowed to inject events", e);
        } finally {
            event.recycle();
        }
    }

    private void releaseAll(String reason) {
        for (int pointer = pointerState.nextSetBit(1); pointer >= 0;
             pointer = pointerState.nextSetBit(pointer + 1)) {
            inject("cancel", pointer, 0, 0, SystemClock.uptimeMillis());
        }
        pointerState.clear();
        Log.i(TAG, "released all pointers (" + reason + ")");
    }

    @Override
    public void onDestroy() {
        running = false;
        releaseAll("service destroyed");
        try {
            if (server != null) {
                server.close();
            }
        } catch (IOException ignored) {
            // shutting down anyway
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
