import SeedSpec.Core

namespace SeedSpec

structure Frame where
  binder : String
  value : Form
  deriving DecidableEq, Repr

/-- Frames apply chronologically, not as a recursively resolved lexical map. -/
def materialize (frames : List Frame) (body : Form) : Form :=
  frames.foldl (fun term frame => subst frame.binder frame.value term) body

def survive (binder : String) (pending : List Frame) : List Frame :=
  pending.filter (fun frame => decide (binder ≠ frame.binder))

/-- Matches deferred_curry's erase_if plus push_back when crossing a binder. -/
def push (pending : List Frame) (frame : Frame) : List Frame :=
  survive frame.binder pending ++ [frame]

@[simp] theorem materialize_cons (frame : Frame) (frames : List Frame) (body : Form) :
    materialize (frame :: frames) body =
      materialize frames (subst frame.binder frame.value body) := rfl

@[simp] theorem survive_cons (binder : String) (frame : Frame) (frames : List Frame) :
    survive binder (frame :: frames) =
      if binder = frame.binder then survive binder frames else frame :: survive binder frames := by
  by_cases shadow : binder = frame.binder <;> simp [survive, shadow]

theorem materialize_abs (frames : List Frame) (binder : String) (body : Form) :
    materialize frames (.abs (some binder) body) =
      .abs (some binder) (materialize (survive binder frames) body) := by
  induction frames generalizing body with
  | nil => rfl
  | cons frame frames ih =>
      by_cases shadow : binder = frame.binder
      · simpa [materialize_cons, survive_cons, subst, shadow] using ih body
      · simpa [materialize_cons, survive_cons, subst, shadow] using ih (subst frame.binder frame.value body)

theorem materialize_push (pending : List Frame) (frame : Frame) (body : Form) :
    materialize (push pending frame) body =
      subst frame.binder frame.value (materialize (survive frame.binder pending) body) := by
  simp [push, materialize, List.foldl_append]

def spine (frames : List Frame) (body : Form) : Form :=
  frames.foldr (fun frame term => .abs (some frame.binder) term) body

/-- The eager reference: substitute through each remaining abstraction in turn.
It consumes exactly the supplied arguments, without demanding the final body. -/
def applyArgs (term : Form) : List Form → Form
  | [] => term
  | argument :: rest =>
      match term with
      | .abs binder body => applyArgs (betaBody binder argument body) rest
      | other => applyArgs (.app other argument) rest

/-- General fusion law, for arbitrary finite bodies and replacement terms.
This is the substitution/materialization stage. C++ has additional eligibility
guards before it may skip intermediate reducer dispatch. -/
theorem curry_fusion (frames pending : List Frame) (body : Form) :
    applyArgs (materialize pending (spine frames body)) (frames.map Frame.value) =
      materialize (frames.foldl push pending) body := by
  induction frames generalizing pending with
  | nil => rfl
  | cons frame frames ih =>
      simp only [spine, List.foldr_cons, List.map_cons, materialize_abs,
        applyArgs, betaBody, List.foldl_cons]
      rw [← materialize_push]
      exact ih (push pending frame)

theorem eager_eq_deferred (frames : List Frame) (body : Form) :
    applyArgs (spine frames body) (frames.map Frame.value) =
      materialize (frames.foldl push []) body :=
  curry_fusion frames [] body

/-- Equality survives any subsequent observer, including a state/effect/error
observer. This does not specify that observer or prove a host implementation. -/
theorem observer_conformance (frames : List Frame) (body : Form)
    (observe : Form → α) :
    observe (applyArgs (spine frames body) (frames.map Frame.value)) =
      observe (materialize (frames.foldl push []) body) :=
  congrArg observe (eager_eq_deferred frames body)

end SeedSpec
