import '@testing-library/jest-dom/vitest'
import { render, screen } from '@testing-library/react'
import { describe, expect, it } from 'vitest'
import { Chessboard } from './Chessboard'

describe('Chessboard', () => {
  it('renders only backend-provided FEN snapshots', () => {
    render(<Chessboard fen="8/8/8/8/8/8/4K3/7k w - - 0 1" />)
    expect(screen.getByRole('img', { name: 'Current game position' })).toBeInTheDocument()
    expect(screen.getByText('♔')).toBeInTheDocument()
    expect(screen.getByText('♚')).toBeInTheDocument()
  })
})
