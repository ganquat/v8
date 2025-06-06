// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Flags: --allow-natives-syntax
// Files: tools/clusterfuzz/foozzie/v8_mock.js
// Files: tools/clusterfuzz/foozzie/v8_mock_archs.js

// Test foozzie architecture-specific mocks for differential fuzzing.

// Test suppressions for Math.pow precision differences.
assertEquals(61180.206355, Math.pow(35, 3.1));
assertEquals(1.2717347483e+29, Math.pow(3, 61));
assertEquals(8.3535164127e-237, Math.pow(Math.E, -543.5899844621109));
