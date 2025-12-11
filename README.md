# Scheduler Execution Semantics

Terms: 

Gauss-Seidel co-simulation; 
fixed-point co-simulation; 
relaxation scheduler; (probably from relaxation as in the convergence?)

Temporal barrier: The Active -> NBA (Effects) ||| 

```pseudocode
forever {
    t = next_time(min(cosim_next_time(), model_next_time()))

    forever {
        model_initial_state = sample(model)
        cosim_initial_state = cosim_state

        foreach event in cosim_events(model_initial_state, cosim_state, t) {
            effects.push(
                cosim_events.front().handler(model_initial_state, cosim_state, t)
            )
        }

        foreach effect in effects {
            model_inputs, cosim_state = effects.front().apply()
        }

        model_eval(model_inputs)

        moddel_updated_state = sample(model)

        if (model_updated_state == model_state) &&
           (cosim_initial_state == cosim_state) break;
    }
}

```

PHASE 1: COMMIT (with edge capture)
    
    # Inputs: capture transition, apply write
    for each InputPort p:
        p.prev = p.current           # Remember old value
        if p.dirty:
            *p.ptr = p.pending       # Apply to DUT
            p.current = p.pending
            p.dirty = false
    
    # Internals: same pattern
    for each InternalSignal s:
        s.prev = s.current
        if s.dirty:
            s.current = s.pending
            s.dirty = false

PHASE 2: EVAL
    
    model.eval()                     # DUT reacts to inputs
                                     # (Verilated clock toggles here if --timing)

PHASE 3: SAMPLE (capture model outputs)
    
    for each OutputPort p:
        p.prev = p.sampled
        p.sampled = *p.ptr

PHASE 4: REACT (unified triggering)
    
    # ALL signal types can trigger processes:
    
    for each InputPort p:            # C++ clock case
        if p.changed(): trigger p.dependents
    
    for each InternalSignal s:       # Derived clock case
        if s.changed(): trigger s.dependents
    
    for each OutputPort p:           # Verilated clock case
        if p.changed(): trigger p.dependents
    
    # Run triggered processes
    for each Process:
        if triggered: callback()

PHASE 5: CONVERGENCE
    
    stable = !any(InputPort.dirty) && !any(InternalSignal.dirty)

