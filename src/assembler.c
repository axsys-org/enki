

n_locals = 1 + law->arity
d = n_locals - 1 
Nat(2) -> pick slot 0
Nat(1) -> pick slot 1
Nat(0) -> pick slot 2
emit_const put into laws const table
so it does both got it store then pish const at the index we just stored at

compile_value(body, d):
  if body is small nat b:
    if b <= d:
      emit OP_PICK (d - b) // copies value to to top of stack of value stack[bas_frame + k] 
    else:
      emit_const body

  else if body is heap ref:
    switch tag:
      PIN:
        emit_const body

      LAW:
        emit_const body

      NAT:
        emit_const body

      APP:
        items = flatten_app(body)

        if items == [0, x]:
          emit_const x

        else if items == [0, f, x]:
          compile_value(f, d)
          compile_value(x, d)
          emit OP_APPLY

        else if items == [1, v, k]:
          TODO bind/thunk/update

        else:
          emit_const body
