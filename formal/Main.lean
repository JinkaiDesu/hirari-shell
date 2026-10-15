import SeedSpec

open SeedSpec

def source : Form → String
  | .unit => "()"
  | .symbol x => x
  | .name t => "-(" ++ source t ++ ")"
  | .opaque text => text
  | .app f a => "(" ++ source f ++ " " ++ source a ++ ")"
  | .abs binder body =>
      "(" ++ binder.getD "_" ++ "() " ++ source body ++ ")"

def ids : List String := ["x", "y", "z"]
def values : List Form :=
  [.symbol "x", .symbol "y", .symbol "z", .name (.symbol "x"),
   .unit, .opaque "3", .opaque "\"constant\"", .abs (some "x") (.symbol "x")]
def stableValues : List Form :=
  [.symbol "x", .symbol "y", .symbol "z", .name (.symbol "x"),
   .opaque "3", .opaque "-9007199254740993"]

def bodies : List Form :=
  [.symbol "x", .symbol "y", .symbol "z", .unit,
   .app (.symbol "x") (.symbol "y"), .app (.symbol "y") (.symbol "x"),
   .app (.symbol "f") (.app (.symbol "x") (.symbol "y")),
   .name (.symbol "x"), .name (.app (.symbol "x") (.symbol "y")),
   .opaque "\"constant\"", .opaque "9223372036854775807",
   .opaque "-9007199254740993", .opaque "3.14",
   .abs (some "x") (.symbol "x"), .abs (some "y") (.symbol "x"),
   .abs (some "z") (.app (.symbol "x") (.symbol "y")),
   .abs none (.symbol "x")]

def row (kind binder input argument answer : String) : String :=
  String.intercalate "\t" [kind, binder, input, argument, answer] ++ "\n"

def substitutionRows : List String := Id.run do
  let mut rows := []
  for x in ids do
    for body in bodies do
      for value in values do
        rows := row "S" x (source body) (source value) (source (subst x value body)) :: rows
  return rows.reverse

def reductionRow (frames : List Frame) (body : Form) : Except String String := do
  let term := (frames.map Frame.value).foldl Form.app (spine frames body)
  let expected <- match normalize 256 term with
    | some answer => pure answer
    | none => throw "Conformance generator exhausted its kernel fuel"
  -- Flat argv exposes the real C++ fusion path; the model uses left application.
  let input := "(" ++ source (spine frames body) ++ " " ++
    String.intercalate " " (frames.map (fun f => source f.value)) ++ ")"
  pure (row "R" "" input "" (source expected))

def pairRows : Except String (List String) := do
  let mut rows := []
  for x in ids do
    for y in ids do
      for body in bodies do
        for a in stableValues do
          for b in stableValues do
            let next <- reductionRow [Frame.mk x a, Frame.mk y b] body
            rows := next :: rows
  return rows.reverse

def tripleRows : Except String (List String) := do
  let mut rows := []
  for x in ids do
    for y in ids do
      for z in ids do
        for a in ids do
          for b in ids do
            for c in ids do
              for body in bodies.take 6 do
                let next <- reductionRow
                  [Frame.mk x (.symbol a), Frame.mk y (.symbol b), Frame.mk z (.symbol c)] body
                rows := next :: rows
  return rows.reverse

def main (args : List String) : IO Unit := do
  let [path] := args | throw (IO.userError "Usage: lake env lean --run Main.lean CORPUS.tsv")
  let rows <- match (do
      let pairs <- pairRows
      let triples <- tripleRows
      pure (substitutionRows ++ pairs ++ triples) : Except String (List String)) with
    | .ok rows => pure rows
    | .error message => throw (IO.userError message)
  IO.FS.writeFile path ("# seed-lean-conformance-v1\n" ++ String.join rows)
  IO.println s!"LEAN_CORPUS_OK {rows.length} cases"
