# Change requests

## Phase 0: Remove newline
- Remove "Newline" option and associated behavor.

## Phase 1: Tweak Text-view

-Move the font coloring logic out of "Garbage" mode, it's generic with few exceptions.

- Rename garbage option (and associated variables/methods/code) to "StringMode" (as that's currently what it does the best.)


- Add RadioGroup
  - StringMode : The same behaivor as before (remove checkbox, add radiobutton)
  - ByteMode : Show _ALL_ chracters as HEX boxes, also printable, no emoji substitution, no special-case for null (it should be 00) - But use same coloring mode for all of them. Wrap is always on in this mode.

- ByteMode only option: "Bytes/Line"
  - 8
  - 16
  - 32
  - 64
  - Auto : Fill width of text-view, then break (Same logic as "Wrap")

Make the background under the "newline" emoji black for better contrast.


- (StringMode only): A "zero" byte between valid chacacters should still be shown as a box character as it is now, but special case it so that the box used is the one with a single zero in it (for better readability) and make the color of that box green and the background black.

- Add to the "hover" behavior in the text-view:
  - Don't reposition/move/scale the bitmap view, but if the pixel representing that character is in view, show it in sharp pink.

- When selecting a range in the text-view:
  - show the pixels representing the bytes in the selected range in cyan.

- Right-clicking on selection shows context menu:
  - Copy
    - Text only : Same as the "StringMode" behavior)
    - Offset + Hex
    - Hex : Standard `NN NN NN...` but no offset first
    - C Header
    - Binary

- When copying the selected range using CTRL+C, replace the 0 byte with ` {null} ` (space,curly-brace-begin,n,u,l,l,curly-brace-end,space).

- Make "Wrap" option hidden when StringMode is off
- Make the "Wrap" option enabled by default.

- Add "Newline" comboBox (option is hidden when StringMode is off)
  - "Text" : Behavior
    - "None" : No bytes will contribute to new lines in the text-view.
    - "NL" : Newline bytes (regardless of the presence of CR and NULL) wil cause a break.
    - "CRLF" : Only CRLF ( \r\n ) will cause a break.
    - "NULL" : Only NULL (0 byte) will cause a break.
    - "NL | CR | NULL" : Any one of (NL, CR, NULL) will cause a break (NOTE: Elaborate rules)
  - Clarification of "Elaborate rules"
    - \n\n will cause two breaks
    - \r\n\r\n will cause two breaks
    - 0 will ONLY cause a break  if the byte _before_ is a printable character (not just valid, but printable).
 - Definition of "break":
   - Break means that after printing the responsible byte, with the appropriate styling, no further characters are added to the current line of the text-view, instead, a new line is inserted.
   - On the new line, output continues as it would otherwise.
   - So, "break" has the same effect on the text-rendering as Wrap has when it is on.

- Add option "Monospace" (widget hidden when StringMode is off)
  - When enabled, a font is used where all glyphs have the same width.

## Phase 2: Tweak Bitmap "Text" mode
- Remove the special behavior for chacaters which are valid but not in a sequence (they should do Grey8 like the rest).

## Phase 3: New bitmap mode "RGBi256"
- On program start, generate an RGB palette with 256 entries (0 = 00,00,00 .. 255 = 255,255,255) each Red, Green, Blue value must have the same number of entries.
- Increase contrast by interlacing color increments (luckily, 255 / 3 = 85.00 )
  - Round 1: increment red
  - Round 2: increment green
  - Round 3: increment blue
  - Round 5: increment red
Int this mode pixel color = rgbPalette[byteValue].

## Phase 4: Architecture overhaul
The current approach of "File/Offset/Before/After" does not afford efficient, robust and simple code.

Move away from pseudo-window created at scan start, to window centered on the first byte of match.

Move away from multi-thread reading, to strinct "1 provider> N consumers".




- ScanTargets: list of
  - filePath
  - fileSize
  - Populated when user selects file or directory

- ScannerThread
  - Has own MatchRecord vector.
  - Loop:
    - Clear MatchRecord vector.
    - Waits for std::binary_semaphore WorkProvided.
      - Scans the range, adds MatchRecords if/when found.
    - Sets std::binary_semaphore ScanComplete.
    - Waits for ResultConsumed

- ReaderThread
 - One ReaderThread started on scan start.
 - fileIdx = 0
 - While fileIdx < scanTargets.length
  - get path & length from ScanTargets
  - Open file for read
  - While file not EOF
    - Allocate ReadBuffer of min( fileSize, BlockSize* )
    - read readBuffer.size from file into readBuffer

    - Create "Workers*2" ScanJobs:
      - Where each scanJob covers roughly readBuffer.size / 2 bytes, except the last one which covers the remaining bytes.
        - ScanJobs start at byte (lastPosition  + 1), first job for a file starts on byte 0.
      - Wait for idle worker
        - Provide job to worker
  - Close file
  - fileIdx++
- No more files: Wait for all workers to be idle
- Add all results from all workers to one list of MatchRecords
- Sort list by fileref and chunkid
- Find overlapping regions (where results in the same file are within 16 MiB of each other)
- Calculate buffer size ( up to 8 MiB before first byte of first result + resultlength + up to 8 MiB after last byte of last result)
- Allocate ReadBuffers for results.
- Open file, fille buffer from bufferOffset util up to 8 MiB after lat byte of last result referencing buffer)


- ReadBuffer
  - Buffer
    - Suitable data-type for byte-access and multi-thread read access.
    - Allocated on creation.
    - Allocated in most efficient way (page-aligned, etc..)
    - Constructor provided size
  - Created by reader-thread during scan-jobs.
  - Created by result-merging on scan-result merge.
  - Is the buffer into virtual viewports points.
  - ScanTargetIdx
  - Offset (this ReadBuffers offset in file (first buffer = 0))


- ScanJob
  - Read-only reference to ReadBuffer
  - fileOffset: Absolute file offset (for result-reporting only
  - offset: Offset in ReadBuffer
  - size: Number of bytes to scan (includes overlap of (searchTermSize-1) bytes at the end only, so duplicate results are impossible)

- MatchRecord contains:
  - ScanTargetIdx
  - threadId
  - offset: Absolute byte offset of first byte of match
  - timestamp_ns


### Virtual viewport and rendering overhaul
The architecture should be overhauled so that both "result table Context column", text-view and bitmap view are computed from virtual viewports (they differ in width of course) into the same binary buffer. This is allowed because they center around the match they display.

Every result is backed by a buffer, and this buffer can have up to 8 MiB before the first byte of the match, and up to 8 MiB after the last byte of the match.
When two results overlap (that is, the last byte of one result is less than 16 MiB away from the first byte of the next result in that same file, then a larger buffer is created),
Any amount of results in the same file can share the same buffer up to 128 MiB in size.
Therefore, the backing result buffer must not be naively iterated, but always looked into via the virtual viewports. Calculating the byte-ranges are easily done using the search result metadata alone.


## Phase 5: "Not empty"
When a file has been selected, load up to 16 MiB of that file into a Readbuffer, set the first byte as result (even if invalid) and refresh the views.