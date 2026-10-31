// The control of gir/inst-raw-types.test: the bug of review R1 (a type
// switch binding lowered from the variable's own type), and one of each
// raw read, in Body code; Lowering code may read raw types.
Operand Body::lowerTypeSwitch(const ast::TypeSwitchStmt *TS) {
  Type *VG = L.lowerType(V->Ty, CC->getBegin());
  S::SignatureType *Sig = F->sig();
  S::Type *T = I.typeOf(E);
  S::Type *N = I.narrowedType(Id);
  auto SI = I.Selections.find(Sel);
  bool B = V->Mode == S::Var::Borrowed;
}

Type *Lowering::lowerTypeOnce(S::Type *T, SourceLoc At) {
  return lowerType(F->Ty, At);
}
