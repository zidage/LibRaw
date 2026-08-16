/* -*- C++ -*-
 * File: nikon_he_bit_reader.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE bitstream reader implementation

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// BitReader methods are inline in nikon_he_bit_reader.h so entropy
// decode can see them. This TU keeps the existing Makefile object list.

#include "nikon_he_bit_reader.h"
