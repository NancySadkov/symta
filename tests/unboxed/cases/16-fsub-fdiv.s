// TS-4.2: typed-float subtract and divide.
fsub_two X^float Y^float = X - Y
fdiv_two X^float Y^float = X / Y
say (fsub_two 10.0 3.0)       // 7.0
say (fsub_two 5.0 5.5)        // -0.5
say (fdiv_two 7.0 2.0)        // 3.5
say (fdiv_two 1.0 4.0)        // 0.25
