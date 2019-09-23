/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2019 Red Hat, Inc.
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

/* Private API between lib and app. */

#pragma once

#include <glib.h>

_RPMOSTREE_EXTERN gboolean
_rpm_ostree_package_diff_lists (GPtrArray  *a,
                                GPtrArray  *b,
                                GPtrArray **out_unique_a,
                                GPtrArray **out_unique_b,
                                GPtrArray **out_modified_a,
                                GPtrArray **out_modified_b,
                                GPtrArray **out_common);
