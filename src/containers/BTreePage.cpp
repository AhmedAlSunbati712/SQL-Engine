#include <BTreePage.h>


void BInternalPage::decode() {
    /**
     * Structure of an internal page
     *  <----------------------------------------------------------------                  4KB                  ---------------------------------------------------------------->
     *  ___________.__________._____________________________.___________________________.__________________________.___________________.___________.___________________________.___________.
     * |           |          |                             |                           |                          |                   |           |                           |           |
     * | 1 Byte    | 2 Bytes  |           2 Bytes           |        2 Bytes -->        |         4 Bytes          |       ....        |    ....   | Key | Right Child PageNum |    ....   |
     * |___________|__________|_____________________________|___________________________|__________________________|___________________|___________|___________________________|___________| PAGE END
     * |           |          |                             |                           |                          |                   |           |                           |           |
     * | Page Type | KeyCount | Free Region Start Offset    | Free Region End Offset    | Leftmost Child Page Num  | Cell Dir Region   |    ....   |     First Internal Cell   |    ....   |
     * |___________|__________|_____________________________|___________________________|__________________________|___________________|___________|___________________________|___________|
     *
     * KeyCount is the number of separator keys currently stored in the page.
     *
     * The cell directory region contains KeyCount 2-byte integers. Each integer stores the
     * offset from the start of the page to the start of an internal cell. The directory entries
     * are kept in sorted key order.
     *
     * Internal Cell layout
     *  __________.______________________.
     * |          |                      |
     * | 8 Bytes  |       4 Bytes        |
     * |__________|______________________|
     * |          |                      |
     * |   Key    | Right Child Page Num |
     * |__________|______________________|
     *
     * Internal pages hold M separator keys and M + 1 child page numbers.
     *
     * The leftmost child page number is stored in the page header. Then each internal cell stores
     * a separator key and the child page number to the right of that separator.
     *
     * All fixed-width numbers are serialized in big-endian format.
     */

}


void BLeafPage::decode() {
    /**
     * Structure of a leaf page
     *  <---------------------------------------------------                     4KB                     --------------------------------------------------->
     *  ___________.__________._____________________________.___________________________.___________________.___________.____________________________.___________.
     * |           |          |                             |                           |                   |           |                            |           |
     * | 1 Byte    |  2 Bytes |           2 Bytes           |        2 Bytes -->        |      ....         |    ....   |  Key | Type | Size | Value |    ....   |
     * |___________|__________|_____________________________|___________________________|___________________|___________|____________________________|___________| PAGE END
     * |           |          |                             |                           |                   |           |                            |           |
     * | Page Type | KeyCount | Free Region Start Offset    | Free Region End Offset    | Cell Dir Region   |    ....   |    First Key-Value Entry   |    ....   |
     * |___________|__________|_____________________________|___________________________|___________________|___________|____________________________|___________|
     *
     * KeyCount is the number of live key-value cells currently stored in the page.
     *
     * The cell directory region contains KeyCount 2-byte integers. Each integer stores the
     * offset from the start of the page to the start of a key-value cell. The directory entries
     * are kept in sorted key order.
     * 
     * Key-Value Entry layout
     *  __________.___________.____________________.__________________________.
     * |          |           |                    |                          |
     * | 8 Bytes  | 1 Byte    |     2   Bytes      |          Variable        | 
     * |__________|___________|____________________|__________________________|
     * |          |           |                    |                          |
     * |   Key    | ValueType |     Value Size     |           Value          |
     * |__________|___________|____________________|__________________________|
     * 
     * Value Size is stored as a 2-byte integer.
     *
     * If the ValueType is an integer type, the Value payload itself may be VarInt-encoded.
     *
     * All fixed-width numbers are serialized in big-endian format.
     */
    

}
