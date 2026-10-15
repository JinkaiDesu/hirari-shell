import Lake
open Lake DSL

package seed_spec where
  moreLeanArgs := #["-DwarningAsError=true"]

@[default_target]
lean_lib SeedSpec
