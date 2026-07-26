const pieces: Record<string, string> = {
  K: '♔',
  Q: '♕',
  R: '♖',
  B: '♗',
  N: '♘',
  P: '♙',
  k: '♚',
  q: '♛',
  r: '♜',
  b: '♝',
  n: '♞',
  p: '♟',
}

function positionFromFen(fen: string): Array<string | undefined> {
  const result: Array<string | undefined> = []
  for (const value of fen.split(' ')[0].replaceAll('/', '')) {
    const empty = Number(value)
    if (Number.isInteger(empty) && empty > 0) {
      result.push(...Array<string | undefined>(empty))
    } else {
      result.push(value)
    }
  }
  return result.slice(0, 64)
}

export function Chessboard({
  fen = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1',
}: {
  fen?: string
}) {
  const position = positionFromFen(fen)
  return (
    <svg
      className="chessboard"
      viewBox="0 0 800 800"
      role="img"
      aria-label="Current game position"
    >
      {position.map((piece, index) => {
        const file = index % 8
        const rank = Math.floor(index / 8)
        const dark = (file + rank) % 2 === 1
        return (
          <g key={index}>
            <rect
              x={file * 100}
              y={rank * 100}
              width="100"
              height="100"
              fill={dark ? '#6374a3' : '#dce2f2'}
            />
            {piece && (
              <text
                x={file * 100 + 50}
                y={rank * 100 + 70}
                textAnchor="middle"
                fontSize="72"
                className="chess-piece"
              >
                {pieces[piece]}
              </text>
            )}
          </g>
        )
      })}
    </svg>
  )
}
