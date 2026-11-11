/**
* Copyright 2016 Comcast Cable Communications Management, LLC
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
 */

package gaad

import (
	"fmt"
)

// This example replaces the payload of element_instance_tag 1 in an
// auxiliary data stream element and then parses the frame again to read the
// new content.
func ExampleRewriteDSE() {
	frame := testAAC()

	rewritten, err := RewriteDSE(frame, []DSEEdit{
		{Tag: 2, Data: []byte("XYZ"), Align: true},
	})
	if err != nil {
		fmt.Println(err)
		return
	}

	adts, err := ParseADTS(rewritten)
	if err != nil {
		fmt.Println(err)
		return
	}
	for _, element := range adts.Data_stream_elements {
		if element.Element_instance_tag == 2 {
			fmt.Println(string(element.Data_stream_byte[2]))
		}
	}
	// Output: XYZ
}
