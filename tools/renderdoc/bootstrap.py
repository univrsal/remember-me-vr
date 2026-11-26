# Runs inside qrenderdoc's embedded Python (started by rdrun.sh): opens the
# capture, runs the user script with `rd` (the renderdoc module), `ctl` (a
# ReplayController) and `args` (list of extra arguments) as globals, then exits
# before the UI opens. stdout/stderr go to $RD_OUT.
import os
import sys
import traceback

out = open(os.environ["RD_OUT"], "w", buffering=1)
sys.stdout = sys.stderr = out
cap = ctl = None
try:
    import renderdoc as rd

    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    result = cap.OpenFile(os.environ["RD_CAPTURE"], "", None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("couldn't open capture: %s" % result)
    if not cap.LocalReplaySupport():
        raise RuntimeError("capture can't be replayed locally (driver %s)" % cap.DriverName())
    result, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("couldn't replay capture: %s" % result)
    script = os.environ["RD_SCRIPT"]
    sys.path.insert(0, os.path.dirname(script))
    sys.path.insert(0, os.environ["RD_TOOLS"])
    args = [a for a in os.environ.get("RD_ARGS", "").split("\x1f") if a]
    code = compile(open(script).read(), script, "exec")
    exec(code, {"__name__": "__main__", "__file__": script, "rd": rd, "ctl": ctl, "args": args})
except SystemExit:
    pass
except BaseException:
    traceback.print_exc()
    out.write("RDRUN-FAILED\n")
finally:
    try:
        if ctl:
            ctl.Shutdown()
        if cap:
            cap.Shutdown()
        rd.ShutdownReplay()
    except Exception:
        pass
    out.flush()
    os._exit(0)
