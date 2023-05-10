package document

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"math/rand"
	"strconv"
	"strings"

	"time"

	"github.com/goccy/go-json"
)

var asciiSpace = [256]uint8{'\t': 1, '\n': 1, '\v': 1, '\f': 1, '\r': 1, ' ': 1}

type Operation int

const (
	OperationPut Operation = iota
	OperationUpdate
	OperationRemove
)

// Id represents a Vespa document ID.
type Id struct {
	id string

	Type         string
	Namespace    string
	Number       *int64
	Group        string
	UserSpecific string
}

func (d Id) Equal(o Id) bool {
	return d.Type == o.Type &&
		d.Namespace == o.Namespace &&
		((d.Number == nil && o.Number == nil) || *d.Number == *o.Number) &&
		d.Group == o.Group &&
		d.UserSpecific == o.UserSpecific
}

func (d Id) String() string { return d.id }

// ParseId parses a serialized document ID string.
func ParseId(serialized string) (Id, error) {
	parts := strings.SplitN(serialized, ":", 4)
	if len(parts) < 4 || parts[0] != "id" {
		return Id{}, parseError(serialized)
	}
	namespace := parts[1]
	if namespace == "" {
		return Id{}, parseError(serialized)
	}
	docType := parts[2]
	if docType == "" {
		return Id{}, parseError(serialized)
	}
	rest := strings.SplitN(parts[3], ":", 2)
	if len(rest) < 2 {
		return Id{}, parseError(serialized)
	}
	options := rest[0]
	userSpecific := rest[1]
	var number *int64
	group := ""
	if strings.HasPrefix(options, "n=") {
		n, err := strconv.ParseInt(options[2:], 10, 64)
		if err != nil {
			return Id{}, parseError(serialized)
		}
		number = &n
	} else if strings.HasPrefix(options, "g=") {
		group = options[2:]
		if len(group) == 0 {
			return Id{}, parseError(serialized)
		}
	} else if options != "" {
		return Id{}, parseError(serialized)
	}
	if userSpecific == "" {
		return Id{}, parseError(serialized)
	}
	return Id{
		id:           serialized,
		Namespace:    namespace,
		Type:         docType,
		Number:       number,
		Group:        group,
		UserSpecific: userSpecific,
	}, nil
}

// Document represents a Vespa document operation.
type Document struct {
	Id        Id
	Condition string
	Fields    []byte
	Operation Operation
	Create    bool
}

type jsonDocument struct {
	IdString  string          `json:"id"`
	PutId     string          `json:"put"`
	UpdateId  string          `json:"update"`
	RemoveId  string          `json:"remove"`
	Condition string          `json:"condition"`
	Fields    json.RawMessage `json:"fields"`
	Create    bool            `json:"create"`
}

// Decoder decodes documents from a JSON structure which is either an array of objects, or objects separated by newline.
type Decoder struct {
	buf   *bufio.Reader
	dec   *json.Decoder
	array bool
	jsonl bool
}

func (d Document) String() string {
	var sb strings.Builder
	switch d.Operation {
	case OperationPut:
		sb.WriteString("put ")
	case OperationUpdate:
		sb.WriteString("update ")
	case OperationRemove:
		sb.WriteString("remove ")
	}
	sb.WriteString(d.Id.String())
	if d.Condition != "" {
		sb.WriteString(", condition=")
		sb.WriteString(d.Condition)
	}
	if d.Create {
		sb.WriteString(", create=true")
	}
	return sb.String()
}

func (d *Decoder) guessMode() error {
	for !d.array && !d.jsonl {
		b, err := d.buf.ReadByte()
		if err != nil {
			return err
		}
		// Skip leading whitespace
		if b < 0x80 && asciiSpace[b] != 0 {
			continue
		}
		switch rune(b) {
		case '{':
			d.jsonl = true
		case '[':
			d.array = true
		default:
			return fmt.Errorf("unexpected token: %q", string(b))
		}
		if err := d.buf.UnreadByte(); err != nil {
			return err
		}
		if err := d.readArrayToken(true); err != nil {
			return err
		}
	}
	return nil
}

func (d *Decoder) readArrayToken(open bool) error {
	if !d.array {
		return nil
	}
	t, err := d.dec.Token()
	if err != nil {
		return err
	}
	if (open && t == json.Delim('[')) || (!open && t == json.Delim(']')) {
		return nil
	}
	return fmt.Errorf("invalid array token: %q", t)
}

func (d *Decoder) Decode() (Document, error) {
	doc, err := d.decode()
	if err != nil && err != io.EOF {
		return Document{}, fmt.Errorf("invalid json at byte offset %d: %w", d.dec.InputOffset(), err)
	}
	return doc, err
}

func (d *Decoder) decode() (Document, error) {
	if err := d.guessMode(); err != nil {
		return Document{}, err
	}
	if !d.dec.More() {
		if err := d.readArrayToken(false); err != nil {
			return Document{}, err
		}
		return Document{}, io.EOF
	}
	doc := jsonDocument{}
	if err := d.dec.Decode(&doc); err != nil {
		return Document{}, err
	}
	return parseDocument(&doc)
}

func NewDecoder(r io.Reader) *Decoder {
	buf := bufio.NewReaderSize(r, 1<<26)
	return &Decoder{
		buf: buf,
		dec: json.NewDecoder(buf),
	}
}

func parseDocument(d *jsonDocument) (Document, error) {
	id := ""
	var op Operation
	if d.IdString != "" {
		op = OperationPut
		id = d.IdString
	} else if d.PutId != "" {
		op = OperationPut
		id = d.PutId
	} else if d.UpdateId != "" {
		op = OperationUpdate
		id = d.UpdateId
	} else if d.RemoveId != "" {
		op = OperationRemove
		id = d.RemoveId
	} else {
		return Document{}, fmt.Errorf("invalid document: missing operation: %v", d)
	}
	docId, err := ParseId(id)
	if err != nil {
		return Document{}, err
	}
	return Document{
		Id:        docId,
		Operation: op,
		Condition: d.Condition,
		Create:    d.Create,
		Fields:    d.Fields,
	}, nil
}

func parseError(value string) error {
	return fmt.Errorf("invalid document: expected id:<namespace>:<document-type>:[n=<number>|g=<group>]:<user-specific>, got %q", value)
}

// Generator is a reader that returns synthetic documents until a given deadline.
type Generator struct {
	Size     int
	Deadline time.Time

	buf     bytes.Buffer
	nowFunc func() time.Time
}

func NewGenerator(size int, deadline time.Time) *Generator {
	return &Generator{Size: size, Deadline: deadline, nowFunc: time.Now}
}

func (g *Generator) randString(size int) string {
	b := make([]byte, size)
	for i := range b {
		b[i] = byte('a' + rand.Intn('z'-'a'+1))
	}
	return string(b)
}

func (g *Generator) Read(p []byte) (int, error) {
	if g.buf.Len() == 0 {
		if !g.nowFunc().Before(g.Deadline) {
			return 0, io.EOF
		}
		fmt.Fprintf(&g.buf, "{\"put\": \"id:test:test::%s\", \"fields\": {\"test\": \"%s\"}}\n", g.randString(8), g.randString(g.Size))
	}
	return g.buf.Read(p)
}

type number interface{ float64 | int64 | int }

func min[T number](x, y T) T {
	if x < y {
		return x
	}
	return y
}

func max[T number](x, y T) T {
	if x > y {
		return x
	}
	return y
}
