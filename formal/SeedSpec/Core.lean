import Std

namespace SeedSpec

/-- A mathematical view of finite canonical products, not a replacement AST.
Opaque payloads represent literals, cells, foreign references and fresh wildcards.
Their encoding and machine bounds are outside this substitution model. -/
inductive Form where
  | unit
  | symbol (identity : String)
  | name (target : Form)
  | opaque (source : String)
  | app (consumer argument : Form)
  | abs (binder : Option String) (body : Form)
  deriving DecidableEq, Repr

/-- Literal substitution, including intentional capture. Replacement terms are
inserted as written, without recursively substituting into the inserted term. -/
def subst (target : String) (replacement : Form) : Form → Form
  | .symbol x => if x = target then replacement else .symbol x
  | .app f a => .app (subst target replacement f) (subst target replacement a)
  | .abs binder body =>
      if binder = some target then .abs binder body
      else .abs binder (subst target replacement body)
  | other => other

def betaBody (binder : Option String) (argument body : Form) : Form :=
  match binder with
  | some x => subst x argument body
  | none => body

/-- Only the weak-head abstraction kernel. No native effects, global lookup,
orientation dispatch, documentation stripping, or execution budget is implied. -/
inductive Step : Form → Form → Prop where
  | beta : Step (.app (.abs binder body) argument) (betaBody binder argument body)
  | head : Step f f' → Step (.app f argument) (.app f' argument)

theorem step_deterministic (left : Step source a) (right : Step source b) : a = b := by
  induction left generalizing b with
  | beta =>
      cases right with
      | beta => rfl
      | head h => cases h
  | head h ih =>
      cases right with
      | beta => cases h
      | head h' => exact congrArg (fun f => Form.app f _) (ih h')

@[simp] theorem subst_unit : subst x v .unit = .unit := rfl
@[simp] theorem subst_name : subst x v (.name t) = .name t := rfl
@[simp] theorem subst_opaque : subst x v (.opaque payload) = .opaque payload := rfl
@[simp] theorem subst_shadow : subst x v (.abs (some x) body) = .abs (some x) body := by
  simp [subst]

theorem identity_beta (v : Form) : Step (.app (.abs (some "x") (.symbol "x")) v) v := by
  simpa [betaBody, subst] using
    (Step.beta (binder := some "x") (body := .symbol "x") (argument := v))

theorem constant_empty_beta (v : Form) : Step (.app (.abs (some "x") .unit) v) .unit := by
  simpa [betaBody] using
    (Step.beta (binder := some "x") (body := .unit) (argument := v))

theorem anonymous_beta (body v : Form) : Step (.app (.abs none body) v) body :=
  Step.beta

theorem intentional_capture :
    subst "x" (.symbol "y") (.abs (some "y") (.symbol "x")) =
      .abs (some "y") (.symbol "y") := by decide

theorem capture_is_observable :
    subst "x" (.symbol "y") (.abs (some "y") (.symbol "x")) ≠
      .abs (some "z") (.symbol "y") := by decide

/-- A supplied value is not recursively rewritten by the same substitution. -/
theorem replacement_is_raw (v : Form) : subst "x" v (.symbol "x") = v := by
  simp [subst]

def free (x : String) : Form → Bool
  | .symbol y => x == y
  | .app f a => free x f || free x a
  | .abs binder body => if binder = some x then false else free x body
  | _ => false

theorem subst_of_not_free (term : Form) (absent : free x term = false) :
    subst x value term = term := by
  induction term with
  | unit => rfl
  | symbol y =>
      simp [free] at absent
      simp [subst, Ne.symm absent]
  | name t => rfl
  | «opaque» payload => rfl
  | app f a ihf iha =>
      simp [free] at absent
      simp [subst, ihf absent.1, iha absent.2]
  | abs binder body ih =>
      by_cases shadow : binder = some x
      · simp [subst, shadow]
      · simp [free, shadow] at absent
        simp [subst, shadow, ih absent]

def pipe (producer consumer : Form) : Form := .app consumer producer
theorem pipe_orientation : pipe b a = .app a b := rfl

/-- Executable counterpart of Step, used only to generate conformance cases. -/
def step : Form → Option Form
  | .app (.abs binder body) argument => some (betaBody binder argument body)
  | .app f argument => (step f).map (fun f' => .app f' argument)
  | _ => none


theorem step_sound (witness : step source = some target) : Step source target := by
  induction source generalizing target with
  | unit => simp [step] at witness
  | symbol x => simp [step] at witness
  | name t => simp [step] at witness
  | «opaque» payload => simp [step] at witness
  | abs binder body => simp [step] at witness
  | app f argument ihf _ =>
      cases f with
      | unit => simp [step] at witness
      | symbol x => simp [step] at witness
      | name t => simp [step] at witness
      | «opaque» payload => simp [step] at witness
      | abs binder body =>
          simp only [step, Option.some.injEq] at witness
          subst target
          exact Step.beta
      | app g b =>
          have mapped : (step (.app g b)).map (fun next => .app next argument) =
              some target := witness
          cases nextStep : step (.app g b) with
          | none => simp [nextStep] at mapped
          | some next =>
              have same : Form.app next argument = target := by
                simpa [nextStep] using mapped
              rw [← same]
              exact Step.head (ihf nextStep)

theorem step_complete (witness : Step source target) : step source = some target := by
  induction witness with
  | beta => rfl
  | @head f f' argument previous ih =>
      cases f with
      | unit => cases previous
      | symbol x => cases previous
      | name t => cases previous
      | «opaque» payload => cases previous
      | abs binder body => cases previous
      | app g b =>
          change (step (.app g b)).map (fun next => Form.app next argument) = some (Form.app f' argument)
          rw [ih]
          rfl

theorem step_iff : step source = some target ↔ Step source target :=
  ⟨step_sound, step_complete⟩
def normalize : Nat → Form → Option Form
  | 0, _ => none
  | fuel + 1, t =>
      match step t with
      | none => some t
      | some next => normalize fuel next

end SeedSpec
