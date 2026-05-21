// Reassigning across types inside `static` is caught by the
// existing TS-3.6 reassign check (already strict regardless
// of static mode).  Static enables the PROPAGATION that puts
// the prior type into GVarsTypes; the check fires when the
// new value's type conflicts.
mixup = static:
  X 5
  X = "hello"
