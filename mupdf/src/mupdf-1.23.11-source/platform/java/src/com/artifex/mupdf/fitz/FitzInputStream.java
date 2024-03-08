// Copyright (C) 2004-2021 Artifex Software, Inc.
//
// This file is part of MuPDF.
//
// MuPDF is free software: you can redistribute it and/or modify it under the
// terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// MuPDF is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
// details.
//
// You should have received a copy of the GNU Affero General Public License
// along with MuPDF. If not, see <https://www.gnu.org/licenses/agpl-3.0.en.html>
//
// Alternative licensing terms are available from the licensor.
// For commercial licensing, see <https://www.artifex.com/> or contact
// Artifex Software, Inc., 39 Mesa Street, Suite 108A, San Francisco,
// CA 94129, USA, for further information.

package com.artifex.mupdf.fitz;

import java.io.IOException;
import java.io.InputStream;

public class FitzInputStream extends InputStream
{
	static {
		Context.init();
	}

	private long pointer;
	private long markpos;
	private boolean closed;

	protected native void finalize();

	public void destroy() {
		finalize();
	}

	private FitzInputStream(long p) {
		pointer = p;
		markpos = -1;
		closed = false;
	}

	public native void mark(int readlimit);
	public native boolean markSupported();
	public native void reset() throws IOException;

	public native int available();

	private native int readByte();
	private native int readArray(byte[] b, int off, int len);
	public int read() {
		return readByte();
	}
	public int read(byte[] b, int off, int len) {
		return readArray(b, off, len);
	}
	public int read(byte[] b) {
		return readArray(b, 0, b.length);
	}

	public native void close() throws IOException;
}
